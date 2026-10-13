// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/ir.h"

namespace wolang {
namespace ir {
namespace {

constexpr std::string_view kMnemonics[] = {
#define WOLANG_IR_OP(Name, Mnemonic, Description) Mnemonic,
#define WOLANG_IR_TERMINATOR(Name, Mnemonic, Description) Mnemonic,
#include "wolang/ir/ir_opcode.inc"
};

static_assert(std::size(kMnemonics) == static_cast<size_t>(Opcode::kCount),
              "every opcode needs a mnemonic");

}  // namespace

std::string_view OpcodeMnemonic(Opcode opcode) {
  const auto index = static_cast<size_t>(opcode);
  return index < std::size(kMnemonics) ? kMnemonics[index]
                                       : std::string_view("<bad opcode>");
}

bool IsTerminator(Opcode opcode) {
  switch (opcode) {
#define WOLANG_IR_TERMINATOR(Name, Mnemonic, Description) \
  case Opcode::k##Name:
#include "wolang/ir/ir_opcode.inc"
      return true;
    default:
      return false;
  }
}

bool ProducesValue(Opcode opcode) {
  return !IsTerminator(opcode) && opcode != Opcode::kStore;
}

const Function* Module::Find(Symbol name) const {
  for (const Function& function : functions) {
    if (function.name == name) {
      return &function;
    }
  }
  return nullptr;
}

}  // namespace ir
}  // namespace wolang
