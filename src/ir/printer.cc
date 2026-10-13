// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/printer.h"

#include <cstdio>

namespace wolang {
namespace ir {
namespace {

class Printer {
 public:
  Printer(const StringInterner& interner,
          const TypeStore& types,
          const Module& module)
      : interner_(interner), types_(types), module_(module) {}

  void PrintFunction(const Function& function) {
    out_.append("fun @");
    out_.append(interner_.Text(function.name));
    out_.push_back('(');
    const std::span<const InstId> params = function.ParamsOf(function.entry());
    for (size_t i = 0; i < params.size(); ++i) {
      if (i != 0) {
        out_.append(", ");
      }
      Name(params[i]);
      out_.append(": ");
      out_.append(types_.Describe(interner_, function.Get(params[i]).type));
    }
    out_.push_back(')');

    const TypeId result = types_.KindOf(function.type) == TypeKind::kFunction
                              ? types_.ElementOf(function.type)
                              : TypeId{};
    if (result.is_valid() && types_.KindOf(result) != TypeKind::kVoid) {
      out_.append(" -> ");
      out_.append(types_.Describe(interner_, result));
    }
    if (!function.effects.empty()) {
      out_.append(" uses ");
      out_.append(types_.Describe(interner_, function.effects));
    }
    out_.append(" {\n");

    for (uint32_t i = 0; i < function.blocks.size(); ++i) {
      PrintBlock(function, BlockId{i});
    }
    out_.append("}\n");
  }

  std::string take() { return std::move(out_); }

 private:
  void Name(InstId id) {
    out_.push_back('%');
    out_.append(std::to_string(id.index));
  }

  void Label(BlockId id) {
    out_.push_back('^');
    out_.append(std::to_string(id.index));
  }

  void PrintBlock(const Function& function, BlockId id) {
    const Block& block = function.Get(id);
    Label(id);
    // The entry block's parameters are the function's, already printed in
    // the signature; every other block lists its own.
    if (id.index != 0 && block.param_count != 0) {
      out_.push_back('(');
      for (uint32_t i = 0; i < block.param_count; ++i) {
        if (i != 0) {
          out_.append(", ");
        }
        Name(block.instructions[i]);
        out_.append(": ");
        out_.append(
            types_.Describe(interner_, function.Get(block.instructions[i]).type));
      }
      out_.push_back(')');
    }
    out_.push_back(':');
    if (!block.label.empty()) {
      out_.append("  // ");
      out_.append(block.label);
    }
    out_.push_back('\n');

    for (size_t i = block.param_count; i < block.instructions.size(); ++i) {
      PrintInst(function, block.instructions[i]);
    }
  }

  void PrintInst(const Function& function, InstId id) {
    const Inst& inst = function.Get(id);
    out_.append("  ");
    if (ProducesValue(inst.opcode)) {
      Name(id);
      out_.append(" = ");
    }
    out_.append(OpcodeMnemonic(inst.opcode));

    switch (inst.opcode) {
      case Opcode::kConstInt:
        out_.push_back(' ');
        out_.append(std::to_string(inst.payload.integer));
        break;
      case Opcode::kConstFloat: {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), " %.17g",
                      inst.payload.floating);
        out_.append(buffer);
        break;
      }
      case Opcode::kConstBool:
        out_.append(inst.payload.boolean ? " true" : " false");
        break;
      case Opcode::kConstString:
        out_.append(" #");
        out_.append(std::to_string(inst.payload.string));
        break;
      case Opcode::kCall: {
        out_.append(" @");
        const uint32_t callee = inst.payload.callee;
        if (callee < module_.functions.size()) {
          out_.append(interner_.Text(module_.functions[callee].name));
        } else {
          out_.append("<unknown>");
        }
        break;
      }
      case Opcode::kExtract:
      case Opcode::kInsert:
        out_.push_back(' ');
        out_.append(std::to_string(inst.field));
        break;
      default:
        break;
    }

    for (size_t i = 0; i < inst.operands_count; ++i) {
      out_.append(i == 0 && !NeedsCommaBeforeFirstOperand(inst.opcode) ? " "
                                                                       : ", ");
      Name(function.operands[inst.operands_begin + i]);
    }

    // A switch prints its cases next to the block each one goes to.
    if (inst.opcode == Opcode::kSwitch) {
      const std::span<const BlockId> targets = function.SuccessorsOf(inst);
      const std::span<const uint64_t> values = function.CasesOf(inst);
      out_.append(" [");
      for (size_t i = 0; i < values.size() && i < targets.size(); ++i) {
        if (i != 0) {
          out_.append(", ");
        }
        out_.append(std::to_string(values[i]));
        out_.append(" -> ");
        Label(targets[i]);
      }
      out_.append("] else ");
      if (!targets.empty()) {
        Label(targets.back());
      }
    } else {
      for (BlockId target : function.SuccessorsOf(inst)) {
        out_.push_back(' ');
        Label(target);
      }
    }

    out_.push_back('\n');
  }

  // Only a call puts a name before its operands, so only a call needs the
  // separator.
  static bool NeedsCommaBeforeFirstOperand(Opcode opcode) {
    switch (opcode) {
      case Opcode::kCall:
      case Opcode::kExtract:
      case Opcode::kInsert:
        return true;
      default:
        return false;
    }
  }

  const StringInterner& interner_;
  const TypeStore& types_;
  const Module& module_;
  std::string out_;
};

}  // namespace

std::string PrintFunction(const StringInterner& interner,
                          const TypeStore& types,
                          const Module& module,
                          const Function& function) {
  Printer printer(interner, types, module);
  printer.PrintFunction(function);
  return printer.take();
}

std::string PrintModule(const StringInterner& interner,
                        const TypeStore& types,
                        const Module& module) {
  Printer printer(interner, types, module);
  for (const Function& function : module.functions) {
    printer.PrintFunction(function);
  }
  return printer.take();
}

}  // namespace ir
}  // namespace wolang
