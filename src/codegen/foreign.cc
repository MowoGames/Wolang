// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/foreign.h"


#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wolang/codegen/arm64/assembler.h"
#include "wolang/codegen/x64/assembler.h"
#include "wolang/codegen/code_block.h"
#include "wolang/codegen/target.h"
#include "wolang/runtime/builtins.h"
#include "wolang/runtime/library.h"

namespace wolang {
namespace codegen {
namespace {

// How many arguments past the registers a call out of the language can
// pass. Twenty-four is more than any C function a person binds by hand
// wants, and the cap exists so that the thunks a program builds stay a
// handful rather than one per shape it happens to call (D154).
constexpr uint32_t kMostSpilled = 24;

#if defined(__aarch64__) || defined(_M_ARM64)

using arm64::Assembler;
using arm64::FloatRegister;
using arm64::Register;
using FWidth = arm64::FloatWidth;
using IWidth = arm64::Width;

constexpr uint32_t kMaxRegisterArguments = 8;

// void Thunk(void* target, const uint64_t* integers,
//            const uint64_t* floats, uint64_t* result, uint64_t flags,
//            const uint64_t* spilled)
//
// Every argument register is loaded whether the callee reads it or not: the
// ABI lets a caller pass more than is taken, and one thunk is worth more
// than a cache of them. A variadic callee is the exception, and the C
// boundary does not admit one.
using Thunk = void (*)(const void*, const uint64_t*, const uint64_t*,
                       uint64_t*, uint64_t, const uint64_t*);

class Caller : public interp::ForeignCaller {
 public:
  bool Build() { return Thunked(0) != nullptr; }

  interp::ForeignOutcome Call(
      std::string_view symbol,
      std::span<const interp::ForeignArgument> arguments,
      bool returns_float,
      uint64_t* result) override {
    uint64_t integers[kMaxRegisterArguments] = {};
    uint64_t floats[kMaxRegisterArguments] = {};
    std::vector<uint64_t> spilled;
    uint32_t integer_count = 0;
    uint32_t float_count = 0;
    for (const interp::ForeignArgument& argument : arguments) {
      if (argument.is_float) {
        if (float_count < kMaxRegisterArguments) {
          floats[float_count++] = argument.bits;
          continue;
        }
      } else if (integer_count < kMaxRegisterArguments) {
        integers[integer_count++] = argument.bits;
        continue;
      }
      // Past the registers this ABI has: the rest go on the stack, in
      // the order they were written (D154).
      spilled.push_back(argument.bits);
    }
    if (spilled.size() > kMostSpilled) {
      return interp::ForeignOutcome::kTooManyArguments;
    }

    const Thunk thunk = Thunked(static_cast<uint32_t>(spilled.size()));
    if (thunk == nullptr) {
      return interp::ForeignOutcome::kTooManyArguments;
    }
    const std::string name(symbol);
    void* target = runtime::LookupSymbol(name);
    if (target == nullptr) {
      target = runtime::FindSymbol(name);
    }
    if (target == nullptr) {
      return interp::ForeignOutcome::kNoSymbol;
    }

    uint64_t raw = 0;
    thunk(target, integers, floats, &raw, returns_float ? 1 : 0,
          spilled.empty() ? nullptr : spilled.data());
    *result = raw;
    return interp::ForeignOutcome::kCalled;
  }

 private:
  // One thunk per number of stack arguments, built the first time that
  // many are passed. Which is how the copies can be a fixed sequence of
  // immediate-offset loads and stores rather than a loop that has to do
  // arithmetic on the stack pointer.
  Thunk Thunked(uint32_t spilled) {
    if (spilled > kMostSpilled) {
      return nullptr;
    }
    if (auto found = thunks_.find(spilled); found != thunks_.end()) {
      return found->second.is_installed()
                 ? reinterpret_cast<Thunk>(
                       const_cast<void*>(found->second.AddressOf(0)))
                 : nullptr;
    }

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
    a.Mov(IWidth::k64, Register::kX11, Register::kX5);

    // Room for the stack arguments, kept a multiple of sixteen because
    // this ABI requires the stack pointer to be.
    const uint32_t room = ((spilled * 8) + 15) / 16 * 16;
    if (room != 0) {
      a.SubImm(IWidth::k64, Register::kSp, Register::kSp, room);
      for (uint32_t i = 0; i < spilled; ++i) {
        a.Ldr(IWidth::k64, Register::kX12, Register::kX11,
              static_cast<int32_t>(i * 8));
        a.Str(IWidth::k64, Register::kX12, Register::kSp,
              static_cast<int32_t>(i * 8));
      }
    }

    for (uint32_t i = 0; i < kMaxRegisterArguments; ++i) {
      a.LdrFloat(FWidth::k64, static_cast<FloatRegister>(i), Register::kX10,
                 static_cast<int32_t>(i * 8));
    }
    for (uint32_t i = 0; i < kMaxRegisterArguments; ++i) {
      a.Ldr(IWidth::k64, static_cast<Register>(i), Register::kX9,
            static_cast<int32_t>(i * 8));
    }
    a.Blr(Register::kX16);

    if (room != 0) {
      // Back to the frame, so that what was saved below it is where this
      // expects to find it.
      a.AddImm(IWidth::k64, Register::kSp, Register::kSp, room);
    }
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

    const std::vector<uint32_t>& words = a.words();
    std::vector<uint8_t> code(words.size() * sizeof(uint32_t));
    if (!words.empty()) {
      std::memcpy(code.data(), words.data(), code.size());
    }
    CodeBlock& block = thunks_[spilled];
    if (!block.Install(code)) {
      return nullptr;
    }
    return reinterpret_cast<Thunk>(const_cast<void*>(block.AddressOf(0)));
  }

  std::map<uint32_t, CodeBlock> thunks_;
};

#elif defined(__x86_64__) || defined(_M_X64)

using x64::Address;
using x64::Assembler;
using x64::Condition;
using x64::Register;
using x64::XmmRegister;
using FWidth = x64::FloatWidth;
using IWidth = x64::Width;

// This ABI passes six integers and eight floats in registers, where arm64
// passes eight of each.
constexpr uint32_t kMaxIntegerArguments = 6;
constexpr uint32_t kMaxFloatArguments = 8;

using Thunk = void (*)(const void*, const uint64_t*, const uint64_t*,
                       uint64_t*, uint64_t, const uint64_t*);

class Caller : public interp::ForeignCaller {
 public:
  bool Build() { return Thunked(0) != nullptr; }

  interp::ForeignOutcome Call(
      std::string_view symbol,
      std::span<const interp::ForeignArgument> arguments,
      bool returns_float,
      uint64_t* result) override {
    uint64_t integers[kMaxIntegerArguments] = {};
    uint64_t floats[kMaxFloatArguments] = {};
    std::vector<uint64_t> spilled;
    uint32_t integer_count = 0;
    uint32_t float_count = 0;
    for (const interp::ForeignArgument& argument : arguments) {
      if (argument.is_float) {
        if (float_count < kMaxFloatArguments) {
          floats[float_count++] = argument.bits;
          continue;
        }
      } else if (integer_count < kMaxIntegerArguments) {
        integers[integer_count++] = argument.bits;
        continue;
      }
      spilled.push_back(argument.bits);
    }
    if (spilled.size() > kMostSpilled) {
      return interp::ForeignOutcome::kTooManyArguments;
    }

    const Thunk thunk = Thunked(static_cast<uint32_t>(spilled.size()));
    if (thunk == nullptr) {
      return interp::ForeignOutcome::kTooManyArguments;
    }
    const std::string name(symbol);
    void* target = runtime::LookupSymbol(name);
    if (target == nullptr) {
      target = runtime::FindSymbol(name);
    }
    if (target == nullptr) {
      return interp::ForeignOutcome::kNoSymbol;
    }

    uint64_t raw = 0;
    thunk(target, integers, floats, &raw, returns_float ? 1 : 0,
          spilled.empty() ? nullptr : spilled.data());
    *result = raw;
    return interp::ForeignOutcome::kCalled;
  }

 private:
  Thunk Thunked(uint32_t spilled) {
    if (spilled > kMostSpilled) {
      return nullptr;
    }
    if (auto found = thunks_.find(spilled); found != thunks_.end()) {
      return found->second.is_installed()
                 ? reinterpret_cast<Thunk>(
                       const_cast<void*>(found->second.AddressOf(0)))
                 : nullptr;
    }

    Assembler a;
    a.Push(Register::kRbp);
    a.Mov(IWidth::k64, Register::kRbp, Register::kRsp);
    // Sixteen bytes, which is also what keeps the stack aligned for the
    // call below -- this ABI requires that and some callees check it.
    a.SubImmediate(IWidth::k64, Register::kRsp, 16);
    // Below the frame pointer rather than at the stack pointer, because
    // pushing the stack arguments moves the stack pointer and these two
    // have to still be findable afterwards (D154).
    a.Store(IWidth::k64, Address{Register::kRbp, -8}, Register::kRcx);
    a.Store(IWidth::k64, Address{Register::kRbp, -16}, Register::kR8);

    // The target and the integer array go where no argument register is,
    // so that loading the arguments cannot overwrite either.
    a.Mov(IWidth::k64, Register::kR10, Register::kRdi);
    a.Mov(IWidth::k64, Register::kR11, Register::kRsi);
    // The floats first, while the pointer to them is still in `rdx`: that
    // register is itself the third integer argument.
    for (uint32_t i = 0; i < kMaxFloatArguments; ++i) {
      a.LoadFloat(FWidth::k64, static_cast<XmmRegister>(i),
                  Address{Register::kRdx, static_cast<int32_t>(i * 8)});
    }
    // Then the stack arguments, while the pointer to them is still in
    // `r9` -- the sixth integer argument register. Pushed back to front,
    // so that the first one ends up at the lowest address, and with a
    // pad when there is an odd number of them so that the stack stays
    // aligned.
    if (spilled != 0) {
      if ((spilled % 2) != 0) {
        a.Push(Register::kRax);
      }
      for (uint32_t i = spilled; i > 0; --i) {
        a.Load(IWidth::k64, Register::kRax,
               Address{Register::kR9, static_cast<int32_t>((i - 1) * 8)});
        a.Push(Register::kRax);
      }
    }
    static constexpr Register kIntegerArguments[kMaxIntegerArguments] = {
        Register::kRdi, Register::kRsi, Register::kRdx,
        Register::kRcx, Register::kR8,  Register::kR9,
    };
    for (uint32_t i = 0; i < kMaxIntegerArguments; ++i) {
      a.Load(IWidth::k64, kIntegerArguments[i],
             Address{Register::kR11, static_cast<int32_t>(i * 8)});
    }
    // How many vector registers hold arguments, which a variadic callee
    // reads out of `al`. Eight is the truth here: all of them are loaded.
    a.MovImmediate(IWidth::k32, Register::kRax, kMaxFloatArguments);
    a.CallRegister(Register::kR10);

    a.Load(IWidth::k64, Register::kRcx, Address{Register::kRbp, -8});
    a.Load(IWidth::k64, Register::kR8, Address{Register::kRbp, -16});
    const x64::Label floating = a.CreateLabel();
    const x64::Label done = a.CreateLabel();
    a.Test(IWidth::k64, Register::kR8, Register::kR8);
    a.Jcc(Condition::kNotEqual, floating);
    a.Store(IWidth::k64, Address{Register::kRcx, 0}, Register::kRax);
    a.Jmp(done);
    a.Bind(floating);
    a.StoreFloat(FWidth::k64, Address{Register::kRcx, 0},
                 XmmRegister::kXmm0);
    a.Bind(done);
    a.Mov(IWidth::k64, Register::kRsp, Register::kRbp);
    a.Pop(Register::kRbp);
    a.Ret();
    a.Resolve();

    CodeBlock& block = thunks_[spilled];
    if (!block.Install(a.bytes())) {
      return nullptr;
    }
    return reinterpret_cast<Thunk>(const_cast<void*>(block.AddressOf(0)));
  }

  std::map<uint32_t, CodeBlock> thunks_;
};

#endif  // the host

}  // namespace

std::unique_ptr<interp::ForeignCaller> MakeForeignCaller() {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__x86_64__) || \
    defined(_M_X64)
  if (!HasBackend(HostTarget())) {
    return nullptr;
  }
  auto caller = std::make_unique<Caller>();
  if (!caller->Build()) {
    return nullptr;
  }
  return caller;
#else
  // One thunk per target, like the emitter. Until there is one for this
  // machine the interpreter cannot leave the language, and says so by
  // trapping rather than by guessing.
  return nullptr;
#endif
}

}  // namespace codegen
}  // namespace wolang
