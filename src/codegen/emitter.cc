// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/emitter.h"

#include <cstring>
#include <string>

namespace wolang {
namespace codegen {

CompiledFunction EmitFunction(Target target,
                              const StringInterner& interner,
                              const TypeStore& types,
                              abi::LayoutStore* layouts,
                              const ir::Module& module,
                              const ir::Function& function,
                              const std::vector<bool>& compilable,
                              const std::vector<uint64_t>& foreign,
                              const std::vector<std::pair<uint64_t,
                                                          uint64_t>>& strings,
                              uint32_t trap_site_base) {
  switch (target) {
    case Target::kArm64:
      return arm64::EmitFunction(interner, types, layouts, module, function,
                                 compilable, foreign, strings,
                                 trap_site_base);
    case Target::kX64:
      return x64::EmitFunction(interner, types, layouts, module, function,
                               compilable, foreign, strings,
                               trap_site_base);
    case Target::kUnknown:
      break;
  }
  CompiledFunction result;
  result.reason = "has no code generator for ";
  result.reason.append(TargetName(target));
  return result;
}

void PatchCall(Target target,
               std::vector<uint8_t>* code,
               uint32_t at,
               uint32_t callee) {
  switch (target) {
    case Target::kArm64: {
      // `bl` carries its displacement in the low 26 bits, counted in
      // instructions from the instruction itself.
      uint32_t word = 0;
      std::memcpy(&word, code->data() + at, sizeof(uint32_t));
      const int32_t distance = (static_cast<int32_t>(callee) -
                                static_cast<int32_t>(at)) /
                               4;
      word |= static_cast<uint32_t>(distance) & 0x03FFFFFFu;
      std::memcpy(code->data() + at, &word, sizeof(uint32_t));
      return;
    }
    case Target::kX64: {
      // `call rel32`: one opcode byte, then a displacement counted from
      // the end of the instruction.
      const int32_t distance = static_cast<int32_t>(callee) -
                               static_cast<int32_t>(at + 5);
      const auto raw = static_cast<uint32_t>(distance);
      std::memcpy(code->data() + at + 1, &raw, sizeof(uint32_t));
      return;
    }
    case Target::kUnknown:
      return;
  }
}

}  // namespace codegen
}  // namespace wolang
