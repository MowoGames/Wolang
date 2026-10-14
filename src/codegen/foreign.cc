// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/foreign.h"

#include <dlfcn.h>

#include <string>
#include <vector>

#include "wolang/codegen/arm64/assembler.h"
#include "wolang/codegen/code_block.h"

namespace wolang {
namespace codegen {
namespace {

#if defined(__aarch64__) || defined(_M_ARM64)

using arm64::Assembler;
using arm64::FloatRegister;
using arm64::Register;
using FWidth = arm64::FloatWidth;
using IWidth = arm64::Width;

constexpr uint32_t kMaxRegisterArguments = 8;

// void Thunk(void* target, const uint64_t* integers,
//            const uint64_t* floats, uint64_t* result, uint64_t flags)
//
// Every argument register is loaded whether the callee reads it or not: the
// ABI lets a caller pass more than is taken, and one thunk is worth more
// than a cache of them. A variadic callee is the exception, and the C
// boundary does not admit one.
using Thunk = void (*)(const void*, const uint64_t*, const uint64_t*,
                       uint64_t*, uint64_t);

class Caller : public interp::ForeignCaller {
 public:
  bool Build() {
    Assembler a;
    a.StpPreIndex(Register::kX29, Register::kX30, Register::kSp, -32);
    a.AddImm(IWidth::k64, Register::kX29, Register::kSp, 0);
    // The result pointer and the flags outlive the call; every register
    // that could hold them is either an argument or caller saved.
    a.Str(IWidth::k64, Register::kX3, Register::kSp, 16);
    a.Str(IWidth::k64, Register::kX4, Register::kSp, 24);
    a.Mov(IWidth::k64, Register::kX16, Register::kX0);
    a.Mov(IWidth::k64, Register::kX9, Register::kX1);
    a.Mov(IWidth::k64, Register::kX10, Register::kX2);

    for (uint32_t i = 0; i < kMaxRegisterArguments; ++i) {
      a.LdrFloat(FWidth::k64, static_cast<FloatRegister>(i), Register::kX10,
                 static_cast<int32_t>(i * 8));
    }
    for (uint32_t i = 0; i < kMaxRegisterArguments; ++i) {
      a.Ldr(IWidth::k64, static_cast<Register>(i), Register::kX9,
            static_cast<int32_t>(i * 8));
    }
    a.Blr(Register::kX16);

    a.Ldr(IWidth::k64, Register::kX9, Register::kSp, 16);
    a.Ldr(IWidth::k64, Register::kX10, Register::kSp, 24);
    const arm64::Label floating = a.CreateLabel();
    const arm64::Label done = a.CreateLabel();
    a.Cbnz(IWidth::k64, Register::kX10, floating);
    a.Str(IWidth::k64, Register::kX0, Register::kX9, 0);
    a.B(done);
    a.Bind(floating);
    a.StrFloat(FWidth::k64, FloatRegister::kV0, Register::kX9, 0);
    a.Bind(done);
    a.LdpPostIndex(Register::kX29, Register::kX30, Register::kSp, 32);
    a.Ret();
    a.Resolve();

    return block_.Install(a.words());
  }

  bool Call(std::string_view symbol,
            std::span<const interp::ForeignArgument> arguments,
            bool returns_float,
            uint64_t* result) override {
    if (!block_.is_installed()) {
      return false;
    }
    const std::string name(symbol);
    void* target = ::dlsym(RTLD_DEFAULT, name.c_str());
    if (target == nullptr) {
      return false;
    }

    uint64_t integers[kMaxRegisterArguments] = {};
    uint64_t floats[kMaxRegisterArguments] = {};
    uint32_t integer_count = 0;
    uint32_t float_count = 0;
    for (const interp::ForeignArgument& argument : arguments) {
      if (argument.is_float) {
        if (float_count >= kMaxRegisterArguments) {
          return false;
        }
        floats[float_count++] = argument.bits;
        continue;
      }
      if (integer_count >= kMaxRegisterArguments) {
        return false;
      }
      integers[integer_count++] = argument.bits;
    }

    const auto thunk =
        reinterpret_cast<Thunk>(const_cast<void*>(block_.AddressOf(0)));
    uint64_t raw = 0;
    thunk(target, integers, floats, &raw, returns_float ? 1 : 0);
    *result = raw;
    return true;
  }

 private:
  CodeBlock block_;
};

#endif  // arm64

}  // namespace

std::unique_ptr<interp::ForeignCaller> MakeForeignCaller() {
#if defined(__aarch64__) || defined(_M_ARM64)
  auto caller = std::make_unique<Caller>();
  if (!caller->Build()) {
    return nullptr;
  }
  return caller;
#else
  return nullptr;
#endif
}

}  // namespace codegen
}  // namespace wolang
