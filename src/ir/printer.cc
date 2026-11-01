// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/printer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolang {
namespace ir {
namespace {

class Printer {
 public:
  Printer(const StringInterner& interner, const TypeStore& types, const Module& module, bool typed = false)
      : interner_(interner), types_(types), module_(module), m_Typed(typed) {}

  void PrintModuleHeader() {
    out_.append("module initializer=");
    out_.append(std::to_string(module_.initializer));
    out_.append(" runaway=");
    out_.append(std::to_string(module_.runaway_item));
    out_.push_back('\n');

    for (const std::string& text : module_.strings) {
      out_.append("string ");
      Quote(text);
      out_.push_back('\n');
    }

    for (const ClassInfo& info : module_.classes) {
      out_.append("class ");
      out_.append(std::to_string(info.token));
      out_.push_back(' ');
      out_.append(PrintType(interner_, types_, info.type));
      out_.append(" [");
      for (size_t i = 0; i < info.methods.size(); ++i) {
        if (i != 0) {
          out_.push_back(' ');
        }
        out_.append(std::to_string(info.methods[i].selector));
        out_.push_back(':');
        out_.append(std::to_string(info.methods[i].function));
      }
      out_.append("]\n");
    }
  }

  void PrintFunction(const Function& function) {
    if (m_Typed) {
      PrintTypedFunction(function);
      return;
    }

    out_.append("fun @");
    out_.append(interner_.Text(function.name));
    out_.push_back('(');
    if (function.is_declaration()) {
      // No entry block, so the parameter types come from the signature.
      const std::vector<TypeId> params = types_.ArgumentsOf(function.type);
      for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
          out_.append(", ");
        }
        out_.append(types_.Describe(interner_, params[i]));
      }
    } else {
      const std::span<const InstId> params =
          function.ParamsOf(function.entry());
      for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
          out_.append(", ");
        }
        Name(params[i]);
        out_.append(": ");
        out_.append(types_.Describe(interner_, function.Get(params[i]).type));
      }
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

    // A declaration has a signature and no body, which is exactly how it
    // reads.
    if (function.is_declaration()) {
      out_.append("\n");
      return;
    }

    out_.append(" {\n");
    for (uint32_t i = 0; i < function.blocks.size(); ++i) {
      PrintBlock(function, BlockId{i});
    }
    out_.append("}\n");
  }

  std::string take() { return std::move(out_); }

 private:
  static void QuoteInto(std::string* target, std::string_view text) {
    static const char kDigits[] = "0123456789abcdef";
    target->push_back('"');
    for (const char c : text) {
      const unsigned char byte = static_cast<unsigned char>(c);
      if (c == '"' || c == '\\') {
        target->push_back('\\');
        target->push_back(c);
      } else if (c == '\n') {
        target->append("\\n");
      } else if (c == '\r') {
        target->append("\\r");
      } else if (c == '\t') {
        target->append("\\t");
      } else if (byte < 0x20 || byte > 0x7e) {
        target->append("\\x");
        target->push_back(kDigits[byte >> 4]);
        target->push_back(kDigits[byte & 15]);
      } else {
        target->push_back(c);
      }
    }
    target->push_back('"');
  }

  void Quote(std::string_view text) { QuoteInto(&out_, text); }

  void PrintTypedFunction(const Function& function) {
    out_.append("fun @");
    Quote(interner_.Text(function.name));
    out_.append(" : ");
    out_.append(PrintType(interner_, types_, function.type));

    if (!function.effects.empty()) {
      out_.append(" uses ");
      bool first = true;
      for (EffectId effect : function.effects.effects()) {
        if (!first) {
          out_.append(", ");
        }
        first = false;
        out_.append(types_.EffectText(interner_, effect));
      }
    }

    std::string attributes;
    if (function.effects_known) {
      attributes.append("known");
    }
    if (function.is_foreign()) {
      if (!attributes.empty()) {
        attributes.push_back(' ');
      }
      attributes.append("foreign=");
      QuoteInto(&attributes, interner_.Text(function.foreign_symbol));
    }
    if (function.is_task) {
      attributes.append(attributes.empty() ? "task" : " task");
    }
    if (function.is_comptime) {
      attributes.append(attributes.empty() ? "comptime" : " comptime");
    }
    if (function.is_primitive_task) {
      attributes.append(attributes.empty() ? "primtask" : " primtask");
    }
    if (function.self_type.is_valid()) {
      if (!attributes.empty()) {
        attributes.push_back(' ');
      }
      attributes.append("self=");
      attributes.append(PrintType(interner_, types_, function.self_type));
    }
    if (function.source_file != Function::kNoSource || function.source_item != Function::kNoSource || function.source_slot != 0) {
      if (!attributes.empty()) {
        attributes.push_back(' ');
      }
      attributes.append("src=");
      attributes.append(std::to_string(function.source_file));
      attributes.push_back(':');
      attributes.append(std::to_string(function.source_item));
      attributes.push_back(':');
      attributes.append(std::to_string(function.source_slot));
    }
    if (!attributes.empty()) {
      out_.append(" [");
      out_.append(attributes);
      out_.push_back(']');
    }

    if (function.is_declaration()) {
      out_.push_back('\n');
      return;
    }

    out_.append(" {\n");
    for (uint32_t i = 0; i < function.blocks.size(); ++i) {
      PrintBlock(function, BlockId{i});
    }
    out_.append("}\n");
  }

  void PrintTypedHead(const Inst& inst) {
    switch (inst.opcode) {
      case Opcode::kConstInt:
        out_.push_back(' ');
        out_.append(std::to_string(inst.payload.integer));
        break;
      case Opcode::kConstFloat: {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), " %.17g", inst.payload.floating);
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
      case Opcode::kSpawnCall:
      case Opcode::kWaitCall:
      case Opcode::kCall:
      case Opcode::kFuncAddress: {
        const uint32_t callee = inst.payload.callee;
        out_.append(" @");
        Quote(callee < module_.functions.size() ? interner_.Text(module_.functions[callee].name) : std::string_view("<unknown>"));
        out_.push_back('#');
        out_.append(std::to_string(callee));
        break;
      }
      case Opcode::kExtract:
      case Opcode::kInsert:
      case Opcode::kFieldAddress:
      case Opcode::kMakeVariant:
      case Opcode::kVariantPayload:
      case Opcode::kVirtualCall:
      case Opcode::kSoaAddress:
        out_.push_back(' ');
        out_.append(std::to_string(inst.field));
        break;
      case Opcode::kTypeSize:
        out_.append(" : ");
        out_.append(PrintType(interner_, types_, TypeId{static_cast<uint32_t>(inst.payload.integer)}));
        break;
      case Opcode::kAlloca:
        out_.append(" slots ");
        out_.append(std::to_string(inst.payload.slots));
        break;
      case Opcode::kClassTable:
        out_.append(" class ");
        out_.append(std::to_string(inst.payload.klass));
        break;
      default:
        break;
    }
  }

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
    // A pass removes a block by emptying it, so an empty block is a deleted
    // one and has nothing to show.
    if (block.instructions.empty()) {
      return;
    }
    Label(id);
    // The entry block's parameters are the function's, already printed in
    // the signature; every other block lists its own.
    if ((id.index != 0 || m_Typed) && block.param_count != 0) {
      out_.push_back('(');
      for (uint32_t i = 0; i < block.param_count; ++i) {
        if (i != 0) {
          out_.append(", ");
        }
        Name(block.instructions[i]);
        out_.append(": ");
        const TypeId param_type = function.Get(block.instructions[i]).type;
        out_.append(m_Typed ? PrintType(interner_, types_, param_type) : types_.Describe(interner_, param_type));
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

    if (m_Typed) {
      PrintTypedHead(inst);
    }

    switch (m_Typed ? Opcode::kCount : inst.opcode) {
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
      case Opcode::kSpawnCall:
      case Opcode::kWaitCall:
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
      case Opcode::kFieldAddress:
        out_.push_back(' ');
        out_.append(std::to_string(inst.field));
        break;
      case Opcode::kTypeSize:
        // Which type is being measured: the whole content of the
        // instruction, so a dump that left it out said nothing.
        out_.append(" : ");
        out_.append(types_.Describe(
            interner_,
            TypeId{static_cast<uint32_t>(inst.payload.integer)}));
        break;
      case Opcode::kAlloca:
      case Opcode::kUndef:
      case Opcode::kConstNull:
      case Opcode::kMakeStruct:
      case Opcode::kMakeArray:
        // These have no operand to infer a type from, so the dump has to
        // say it or a reader cannot tell what the value is.
        out_.append(" : ");
        out_.append(types_.Describe(interner_, inst.type));
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

    if (m_Typed) {
      if (inst.is_unchecked) {
        out_.append(" !unchecked");
      }
      if (ProducesValue(inst.opcode) && inst.type.is_valid()) {
        out_.append(" : ");
        out_.append(PrintType(interner_, types_, inst.type));
      }
    }

    out_.push_back('\n');
  }

  // Only a call puts a name before its operands, so only a call needs the
  // separator.
  static bool NeedsCommaBeforeFirstOperand(Opcode opcode) {
    switch (opcode) {
      case Opcode::kSpawnCall:
      case Opcode::kWaitCall:
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
  bool m_Typed = false;
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

std::string PrintModuleTyped(const StringInterner& interner, const TypeStore& types, const Module& module) {
  Printer printer(interner, types, module, true);
  printer.PrintModuleHeader();
  for (const Function& function : module.functions) {
    printer.PrintFunction(function);
  }
  return printer.take();
}

std::string PrintType(const StringInterner& interner, const TypeStore& types, TypeId type) {
  if (!type.is_valid()) {
    return "invalid";
  }

  const Type& described = types.Get(type);
  switch (described.kind) {
    case TypeKind::kVoid:
      return "void";
    case TypeKind::kNever:
      return "never";
    case TypeKind::kBool:
      return "bool";
    case TypeKind::kChar:
      return "char";
    case TypeKind::kString:
      return "Text";
    case TypeKind::kStr:
      return "text";
    case TypeKind::kInt:
    case TypeKind::kFloat:
      return types.Describe(interner, type);
    case TypeKind::kOptional:
      return "opt(" + PrintType(interner, types, described.element) + ")";
    case TypeKind::kPointer:
      return std::string(described.is_mutable ? "ptrmut(" : "ptr(") + PrintType(interner, types, described.element) + ")";
    case TypeKind::kReference:
      return std::string(described.is_mutable ? "refmut(" : "ref(") + PrintType(interner, types, described.element) + ")";
    case TypeKind::kSlice:
      return std::string(described.is_soa ? "soaslice(" : "slice(") + PrintType(interner, types, described.element) + ")";
    case TypeKind::kArray:
      return std::string(described.is_soa ? "soaarray(" : "array(") + std::to_string(described.length) + ", " + PrintType(interner, types, described.element) + ")";
    case TypeKind::kTuple: {
      std::string text = "tuple(";
      bool first = true;
      for (TypeId element : types.ArgumentsOf(type)) {
        if (!first) {
          text.append(", ");
        }
        first = false;
        text.append(PrintType(interner, types, element));
      }
      text.push_back(')');
      return text;
    }
    case TypeKind::kFunction: {
      std::string text = types.IsKept(type) ? "fnkept(" : "fn(";
      bool first = true;
      for (TypeId param : types.ArgumentsOf(type)) {
        if (!first) {
          text.append(", ");
        }
        first = false;
        text.append(PrintType(interner, types, param));
      }
      text.append(") -> ");
      text.append(described.element.is_valid() ? PrintType(interner, types, described.element) : std::string("void"));
      return text;
    }
    default:
      return "`" + types.Describe(interner, type) + "`";
  }
}

}  // namespace ir
}  // namespace wolang
