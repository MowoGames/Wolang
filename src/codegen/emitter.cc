// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/emitter.h"

#include <cstring>
#include <string>

#include "wolang/codegen/arm64/assembler.h"

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

void PatchAddress(Target target,
                  std::vector<uint8_t>* code,
                  uint32_t at,
                  uint64_t address) {
  switch (target) {
    case Target::kArm64: {
      // Four instructions, rebuilt rather than edited: the assembler
      // already knows how to write this sequence and an encoder written
      // twice is an encoder that disagrees with itself. Which register to
      // write into comes from the placeholder, so the emitter stays free
      // to choose it.
      uint32_t first = 0;
      std::memcpy(&first, code->data() + at, sizeof(first));
      arm64::Assembler patch;
      patch.MovAddress(static_cast<arm64::Register>(first & 0x1Fu),
                       address);
      const std::vector<uint32_t>& words = patch.words();
      std::memcpy(code->data() + at, words.data(),
                  words.size() * sizeof(uint32_t));
      return;
    }
    case Target::kX64:
      // `movabs`: two bytes of opcode, then the value.
      std::memcpy(code->data() + at + 2, &address, sizeof(address));
      return;
    case Target::kUnknown:
      return;
  }
}

}  // namespace codegen
}  // namespace wolang
