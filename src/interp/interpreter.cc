// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/interp/interpreter.h"

#include "wolang/interp/intrinsics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace wolang {
namespace interp {
namespace {


// One activation. Values are indexed by instruction, which is what makes
// "the value of %7" a lookup.
struct Frame {
  const ir::Function* function = nullptr;
  std::vector<Value> values;
  // Storage behind every `alloca` in this activation.
  std::vector<Value> slots;
};

class Interpreter {
 public:
  Interpreter(const StringInterner& interner,
              const TypeStore& types,
              const ir::Module& module,
              const Limits& limits,
              Output* output,
              ForeignCaller* foreign)
      : interner_(interner),
        types_(types),
        module_(module),
        limits_(limits),
        output_(output),
        foreign_(foreign) {}

  Outcome Run(const ir::Function& function, std::vector<Value> arguments) {
    Outcome outcome;
    const Value result = Call(function, std::move(arguments), &outcome);
    if (outcome.ok()) {
      outcome.value = result;
    }
    return outcome;
  }

 private:
  Value Trap(Outcome* outcome,
             TrapKind kind,
             std::string where,
             std::string message) {
    if (outcome->ok()) {
      outcome->trap = kind;
      outcome->where = std::move(where);
      outcome->message = std::move(message);
    }
    return Value();
  }

  std::string At(const ir::Function& function, ir::InstId id) const {
    std::string result = "@";
    result.append(interner_.Text(function.name));
    result.append(":%");
    result.append(std::to_string(id.index));
    return result;
  }

  Value Call(const ir::Function& function,
             std::vector<Value> arguments,
             Outcome* outcome) {
    if (function.is_foreign()) {
      return CallForeign(function, arguments, outcome);
    }
    if (function.is_declaration()) {
      // A declaration whose name is an intrinsic is implemented by the host.
      // That is the platform floor: everything the language can say about
      // itself is written in Wolang, and this is what is left.
      const Intrinsic intrinsic =
          LookupIntrinsic(interner_.Text(function.name));
      if (intrinsic != Intrinsic::kNone) {
        Value result = CallIntrinsic(intrinsic, arguments, output_);
        if (!result.is_undefined()) {
          return result;
        }
        return Trap(outcome, TrapKind::kUndefinedValue,
                    "@" + std::string(interner_.Text(function.name)),
                    "was given arguments it cannot take");
      }
      return Trap(outcome, TrapKind::kNoBody,
                  "@" + std::string(interner_.Text(function.name)),
                  "has no body to run");
    }
    if (depth_ >= limits_.depth) {
      return Trap(outcome, TrapKind::kStepLimit,
                  "@" + std::string(interner_.Text(function.name)),
                  "call depth limit reached");
    }

    Frame frame;
    frame.function = &function;
    frame.values.assign(function.instructions.size(), Value());

    const std::span<const ir::InstId> params =
        function.ParamsOf(function.entry());
    for (size_t i = 0; i < params.size(); ++i) {
      frame.values[params[i].index] =
          i < arguments.size() ? arguments[i] : Value();
    }

    ++depth_;
    const Value result = Execute(&frame, outcome);
    --depth_;
    return result;
  }

  // Out of the language, through the thunk the host handed over. The
  // arguments are reduced to bits and a register file, which is all the C
  // ABI needs; the checker already made sure every type at the boundary has
  // a representation on both sides (`E0802`).
  Value CallForeign(const ir::Function& function,
                    const std::vector<Value>& arguments,
                    Outcome* outcome) {
    const std::string symbol(interner_.Text(function.foreign_symbol));
    if (foreign_ == nullptr) {
      return Trap(outcome, TrapKind::kNoBody, "@" + symbol,
                  "is `extern`, and this run cannot call out of the "
                  "language");
    }

    std::vector<ForeignArgument> raw;
    raw.reserve(arguments.size());
    for (const Value& argument : arguments) {
      ForeignArgument entry;
      if (argument.kind() == Value::Kind::kFloat) {
        entry.is_float = true;
        if (argument.float_width() == FloatWidth::k64) {
          const double number = argument.floating();
          std::memcpy(&entry.bits, &number, sizeof(number));
        } else {
          // An f32 is passed in the low half of its register.
          const auto number = static_cast<float>(argument.floating());
          uint32_t pattern = 0;
          std::memcpy(&pattern, &number, sizeof(number));
          entry.bits = pattern;
        }
      } else if (argument.kind() == Value::Kind::kBool) {
        entry.bits = argument.boolean() ? 1 : 0;
      } else if (argument.kind() == Value::Kind::kInt) {
        // Sign extended, because that is how a register holds a narrow
        // integer on this ABI.
        entry.bits = argument.is_signed()
                         ? static_cast<uint64_t>(argument.signed_integer())
                         : argument.integer();
      } else {
        return Trap(outcome, TrapKind::kUndefinedValue, "@" + symbol,
                    "was given " + argument.Describe() +
                        ", which has no representation in C");
      }
      raw.push_back(entry);
    }

    const TypeId returned = types_.ElementOf(function.type);
    const Type& described = types_.Get(returned);
    const bool returns_float = described.kind == TypeKind::kFloat;
    uint64_t result = 0;
    if (!foreign_->Call(symbol, raw, returns_float, &result)) {
      return Trap(outcome, TrapKind::kNoBody, "@" + symbol,
                  "is `extern`, and its symbol was not found");
    }

    switch (described.kind) {
      case TypeKind::kVoid:
        return Value::Void();
      case TypeKind::kBool:
        return Value::Bool((result & 1) != 0);
      case TypeKind::kFloat: {
        if (described.float_width == FloatWidth::k64) {
          double number = 0;
          std::memcpy(&number, &result, sizeof(number));
          return Value::Float(number, FloatWidth::k64);
        }
        const auto pattern = static_cast<uint32_t>(result);
        float number = 0;
        std::memcpy(&number, &pattern, sizeof(number));
        return Value::Float(static_cast<double>(number), FloatWidth::k32);
      }
      case TypeKind::kInt:
        return Value::Int(result, described.int_width, described.is_signed);
      case TypeKind::kPointer:
        return Value::Int(result, IntWidth::k64, /*is_signed=*/false);
      default:
        return Trap(outcome, TrapKind::kUndefinedValue, "@" + symbol,
                    "returns something with no representation in C");
    }
  }

  // Walks blocks until a `ret`. Block parameters are filled by the jump that
  // arrives, which is what replaces a phi node.
  Value Execute(Frame* frame, Outcome* outcome) {
    const ir::Function& function = *frame->function;
    ir::BlockId block = function.entry();

    while (function.Contains(block)) {
      const ir::Block& body = function.Get(block);
      for (size_t i = body.param_count; i < body.instructions.size(); ++i) {
        if (++steps_ > limits_.steps) {
          return Trap(outcome, TrapKind::kStepLimit,
                      At(function, body.instructions[i]),
                      "step limit reached");
        }

        const ir::InstId id = body.instructions[i];
        const ir::Inst& inst = function.Get(id);
        if (!ir::IsTerminator(inst.opcode)) {
          frame->values[id.index] = Evaluate(frame, id, inst, outcome);
          if (!outcome->ok()) {
            return Value();
          }
          continue;
        }

        // A terminator either leaves the function or names the next block.
        ir::BlockId next;
        const Value returned = Transfer(frame, id, inst, &next, outcome);
        if (!outcome->ok()) {
          return Value();
        }
        if (!next.is_valid()) {
          return returned;
        }
        block = next;
        break;
      }
    }
    return Value::Void();
  }

  const Value& Operand(Frame* frame, const ir::Inst& inst, size_t index) {
    static const Value kMissing;
    const std::span<const ir::InstId> operands =
        frame->function->OperandsOf(inst);
    if (index >= operands.size() ||
        operands[index].index >= frame->values.size()) {
      return kMissing;
    }
    return frame->values[operands[index].index];
  }

  Value Evaluate(Frame* frame,
                 ir::InstId id,
                 const ir::Inst& inst,
                 Outcome* outcome) {
    const ir::Function& function = *frame->function;
    switch (inst.opcode) {
      case ir::Opcode::kConstInt:
        return Value::Int(inst.payload.integer, WidthOf(inst.type),
                          SignednessOf(inst.type));
      case ir::Opcode::kConstFloat:
        return Value::Float(inst.payload.floating, FloatWidthOf(inst.type));
      case ir::Opcode::kConstBool:
        return Value::Bool(inst.payload.boolean);
      case ir::Opcode::kConstNull:
        return Value::Absent();
      case ir::Opcode::kConstString:
        return inst.payload.string < module_.strings.size()
                   ? Value::Str(module_.strings[inst.payload.string])
                   : Value::Str("");

      case ir::Opcode::kUndef:
        // Reaching one means control went where the front end proved it
        // could not, so it is a trap rather than a zero.
        return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                    "an undefined value was used");

      case ir::Opcode::kAlloca: {
        frame->slots.push_back(Value());
        return Value::Slot(static_cast<uint32_t>(frame->slots.size() - 1));
      }
      case ir::Opcode::kLoad: {
        const Value& slot = Operand(frame, inst, 0);
        if (slot.kind() != Value::Kind::kSlot ||
            slot.slot() >= frame->slots.size()) {
          return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                      "loaded from something that is not a slot");
        }
        const Value& stored = frame->slots[slot.slot()];
        if (stored.is_undefined()) {
          return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                      "read a slot before anything was written to it");
        }
        return stored;
      }
      case ir::Opcode::kStore: {
        const Value& slot = Operand(frame, inst, 0);
        if (slot.kind() != Value::Kind::kSlot ||
            slot.slot() >= frame->slots.size()) {
          return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                      "stored to something that is not a slot");
        }
        frame->slots[slot.slot()] = Operand(frame, inst, 1);
        return Value::Void();
      }

      case ir::Opcode::kMakeStruct:
      case ir::Opcode::kMakeArray: {
        std::vector<Value> elements;
        for (ir::InstId operand : function.OperandsOf(inst)) {
          elements.push_back(operand.index < frame->values.size()
                                 ? frame->values[operand.index]
                                 : Value());
        }
        return Value::Aggregate(std::move(elements));
      }
      case ir::Opcode::kExtract: {
        const Value& aggregate = Operand(frame, inst, 0);
        if (aggregate.kind() != Value::Kind::kAggregate ||
            inst.field >= aggregate.elements().size()) {
          return Trap(outcome, TrapKind::kOutOfBounds, At(function, id),
                      "field " + std::to_string(inst.field) +
                          " is not there");
        }
        return aggregate.elements()[inst.field];
      }
      case ir::Opcode::kInsert: {
        const Value& aggregate = Operand(frame, inst, 0);
        if (aggregate.kind() != Value::Kind::kAggregate ||
            inst.field >= aggregate.elements().size()) {
          return Trap(outcome, TrapKind::kOutOfBounds, At(function, id),
                      "field " + std::to_string(inst.field) +
                          " is not there");
        }
        std::vector<Value> elements = aggregate.elements();
        elements[inst.field] = Operand(frame, inst, 1);
        return Value::Aggregate(std::move(elements));
      }
      case ir::Opcode::kIndex: {
        const Value& array = Operand(frame, inst, 0);
        const Value& index = Operand(frame, inst, 1);
        if (array.kind() != Value::Kind::kAggregate ||
            index.kind() != Value::Kind::kInt) {
          return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                      "indexed something that is not an array");
        }
        if (index.integer() >= array.elements().size()) {
          // The message says what both tiers know. The offending index is
          // a runtime value, and a code generator cannot put it in a
          // string it built while compiling -- so keeping it out is what
          // makes the two tiers' outcomes comparable in full.
          return Trap(outcome, TrapKind::kOutOfBounds, At(function, id),
                      "index is outside a length of " +
                          std::to_string(array.elements().size()));
        }
        return array.elements()[index.integer()];
      }

      case ir::Opcode::kOptionalWrap:
        return Value::Present(Operand(frame, inst, 0));
      case ir::Opcode::kOptionalIsPresent: {
        const Value& optional = Operand(frame, inst, 0);
        return Value::Bool(optional.kind() == Value::Kind::kOptional &&
                           optional.has_value());
      }
      case ir::Opcode::kOptionalValue: {
        const Value& optional = Operand(frame, inst, 0);
        if (optional.kind() != Value::Kind::kOptional ||
            !optional.has_value()) {
          return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                      "read the value of an optional that is absent");
        }
        return optional.inside();
      }

      case ir::Opcode::kCall: {
        if (inst.payload.callee >= module_.functions.size()) {
          return Trap(outcome, TrapKind::kNoBody, At(function, id),
                      "calls a function the module does not have");
        }
        std::vector<Value> arguments;
        for (ir::InstId operand : function.OperandsOf(inst)) {
          arguments.push_back(operand.index < frame->values.size()
                                  ? frame->values[operand.index]
                                  : Value());
        }
        return Call(module_.functions[inst.payload.callee],
                    std::move(arguments), outcome);
      }

      case ir::Opcode::kCast:
        return Convert(frame, id, inst, outcome);

      case ir::Opcode::kLogicalNot: {
        const Value& operand = Operand(frame, inst, 0);
        return Value::Bool(!operand.boolean());
      }

      default:
        return Arithmetic(frame, id, inst, outcome);
    }
  }

  // A numeric conversion written with `as`. The target type is the
  // instruction's, not the operand's -- which is why a cast has to be an
  // instruction rather than something read off a recorded type.
  Value Convert(Frame* frame,
                ir::InstId id,
                const ir::Inst& inst,
                Outcome* outcome) {
    const ir::Function& function = *frame->function;
    const Value& source = Operand(frame, inst, 0);
    const Type& target = types_.Get(inst.type);

    if (target.kind == TypeKind::kInt) {
      const IntWidth width = target.int_width;
      const bool is_signed = target.is_signed;
      if (source.kind() == Value::Kind::kFloat) {
        const double number = std::trunc(source.floating());
        if (!std::isfinite(number) ||
            !FitsAsInteger(number, width, is_signed)) {
          return Trap(outcome, TrapKind::kOverflow, At(function, id),
                      "the value does not fit the integer it was cast to");
        }
        return Value::Int(is_signed ? static_cast<uint64_t>(
                                          static_cast<int64_t>(number))
                                    : static_cast<uint64_t>(number),
                          width, is_signed);
      }
      if (source.kind() != Value::Kind::kInt) {
        return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                    "cast of " + source.Describe());
      }
      // A narrowing cast traps rather than truncating. Overflow is a trap
      // everywhere else in this language, and `as` is a conversion, not a
      // request for the low bits.
      if (!FitsAsInt(source, width, is_signed)) {
        return Trap(outcome, TrapKind::kOverflow, At(function, id),
                    "the value does not fit the integer it was cast to");
      }
      // The sign extended value, not the stored pattern: a negative `i32`
      // widened to `i64` is the same number, and its bits are not.
      return Value::Int(source.is_signed()
                            ? static_cast<uint64_t>(source.signed_integer())
                            : source.integer(),
                        width, is_signed);
    }

    if (target.kind == TypeKind::kFloat) {
      const FloatWidth width = target.float_width;
      if (source.kind() == Value::Kind::kFloat) {
        return Value::Float(source.floating(), width);
      }
      if (source.kind() == Value::Kind::kInt) {
        return Value::Float(source.is_signed()
                                ? static_cast<double>(
                                      source.signed_integer())
                                : static_cast<double>(source.integer()),
                            width);
      }
      return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                  "cast of " + source.Describe());
    }

    // A cast the IR does not distinguish -- a quantity to its own numeric
    // type, or a type to itself.
    return source;
  }

  Value Arithmetic(Frame* frame,
                   ir::InstId id,
                   const ir::Inst& inst,
                   Outcome* outcome) {
    const ir::Function& function = *frame->function;
    const Value& left = Operand(frame, inst, 0);
    const Value& right = Operand(frame, inst, 1);

    if (left.kind() == Value::Kind::kFloat) {
      return FloatArithmetic(inst, left, right);
    }
    if (left.kind() != Value::Kind::kInt) {
      // A comparison on anything else is structural; nothing else applies.
      if (inst.opcode == ir::Opcode::kEq) {
        return Value::Bool(left == right);
      }
      if (inst.opcode == ir::Opcode::kNe) {
        return Value::Bool(left != right);
      }
      return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                  std::string(ir::OpcodeMnemonic(inst.opcode)) +
                      " on " + left.Describe());
    }

    const IntWidth width = left.int_width();
    const bool is_signed = left.is_signed();
    const uint64_t a = left.integer();
    const uint64_t b = right.integer();

    switch (inst.opcode) {
      case ir::Opcode::kAdd:
      case ir::Opcode::kSub:
      case ir::Opcode::kMul: {
        // Overflow is a trap, not a wrap. A language that wraps silently
        // turns an arithmetic bug into a wrong answer somewhere else.
        uint64_t raw = 0;
        bool overflowed = false;
        if (is_signed) {
          const int64_t x = left.signed_integer();
          const int64_t y = right.signed_integer();
          int64_t out = 0;
          overflowed = inst.opcode == ir::Opcode::kAdd
                           ? __builtin_add_overflow(x, y, &out)
                           : inst.opcode == ir::Opcode::kSub
                                 ? __builtin_sub_overflow(x, y, &out)
                                 : __builtin_mul_overflow(x, y, &out);
          raw = static_cast<uint64_t>(out);
          overflowed = overflowed || !FitsSigned(out, width);
        } else {
          uint64_t out = 0;
          overflowed = inst.opcode == ir::Opcode::kAdd
                           ? __builtin_add_overflow(a, b, &out)
                           : inst.opcode == ir::Opcode::kSub
                                 ? __builtin_sub_overflow(a, b, &out)
                                 : __builtin_mul_overflow(a, b, &out);
          raw = out;
          overflowed = overflowed || MaskToWidth(out, width) != out;
        }
        if (overflowed) {
          return Trap(outcome, TrapKind::kOverflow, At(function, id),
                      std::string(ir::OpcodeMnemonic(inst.opcode)) +
                          " overflowed");
        }
        return Value::Int(raw, width, is_signed);
      }

      case ir::Opcode::kDiv:
      case ir::Opcode::kRem: {
        if (b == 0) {
          return Trap(outcome, TrapKind::kDivisionByZero, At(function, id),
                      "divided by zero");
        }
        if (is_signed) {
          const int64_t x = left.signed_integer();
          const int64_t y = right.signed_integer();
          // The one signed division that overflows.
          if (y == -1 && !FitsSigned(-x, width)) {
            return Trap(outcome, TrapKind::kOverflow, At(function, id),
                        "the quotient does not fit");
          }
          const int64_t out =
              inst.opcode == ir::Opcode::kDiv ? x / y : x % y;
          return Value::Int(static_cast<uint64_t>(out), width, is_signed);
        }
        return Value::Int(
            inst.opcode == ir::Opcode::kDiv ? a / b : a % b, width,
            is_signed);
      }

      case ir::Opcode::kBitAnd:
        return Value::Int(a & b, width, is_signed);
      case ir::Opcode::kBitOr:
        return Value::Int(a | b, width, is_signed);
      case ir::Opcode::kBitXor:
        return Value::Int(a ^ b, width, is_signed);
      case ir::Opcode::kShl:
      case ir::Opcode::kShr: {
        if (b >= BitsOfWidth(width)) {
          return Trap(outcome, TrapKind::kOverflow, At(function, id),
                      "shifted by more than the width");
        }
        if (inst.opcode == ir::Opcode::kShl) {
          return Value::Int(a << b, width, is_signed);
        }
        return Value::Int(is_signed
                              ? static_cast<uint64_t>(
                                    left.signed_integer() >>
                                    static_cast<int>(b))
                              : a >> b,
                          width, is_signed);
      }
      case ir::Opcode::kBitNot:
        return Value::Int(~a, width, is_signed);
      case ir::Opcode::kNeg: {
        if (is_signed && !FitsSigned(-left.signed_integer(), width)) {
          return Trap(outcome, TrapKind::kOverflow, At(function, id),
                      "negation overflowed");
        }
        return Value::Int(static_cast<uint64_t>(-left.signed_integer()),
                          width, is_signed);
      }

      case ir::Opcode::kEq:
        return Value::Bool(a == b);
      case ir::Opcode::kNe:
        return Value::Bool(a != b);
      case ir::Opcode::kLt:
        return Value::Bool(is_signed
                               ? left.signed_integer() <
                                     right.signed_integer()
                               : a < b);
      case ir::Opcode::kLe:
        return Value::Bool(is_signed
                               ? left.signed_integer() <=
                                     right.signed_integer()
                               : a <= b);
      case ir::Opcode::kGt:
        return Value::Bool(is_signed
                               ? left.signed_integer() >
                                     right.signed_integer()
                               : a > b);
      case ir::Opcode::kGe:
        return Value::Bool(is_signed
                               ? left.signed_integer() >=
                                     right.signed_integer()
                               : a >= b);

      default:
        return Trap(outcome, TrapKind::kUndefinedValue, At(function, id),
                    std::string(ir::OpcodeMnemonic(inst.opcode)) +
                        " has no execution");
    }
  }

  static Value FloatArithmetic(const ir::Inst& inst,
                               const Value& left,
                               const Value& right) {
    const double a = left.floating();
    const double b = right.floating();
    const FloatWidth width = left.float_width();
    switch (inst.opcode) {
      case ir::Opcode::kAdd:
        return Value::Float(a + b, width);
      case ir::Opcode::kSub:
        return Value::Float(a - b, width);
      case ir::Opcode::kMul:
        return Value::Float(a * b, width);
      // IEEE-754 division by zero is an infinity, not a fault, and that is
      // defined behaviour rather than something to trap on.
      case ir::Opcode::kDiv:
        return Value::Float(a / b, width);
      case ir::Opcode::kNeg:
        return Value::Float(-a, width);
      case ir::Opcode::kEq:
        return Value::Bool(a == b);
      case ir::Opcode::kNe:
        return Value::Bool(a != b);
      case ir::Opcode::kLt:
        return Value::Bool(a < b);
      case ir::Opcode::kLe:
        return Value::Bool(a <= b);
      case ir::Opcode::kGt:
        return Value::Bool(a > b);
      case ir::Opcode::kGe:
        return Value::Bool(a >= b);
      default:
        return Value();
    }
  }

  // Runs a terminator. Sets |next| when control stays in the function.
  Value Transfer(Frame* frame,
                 ir::InstId id,
                 const ir::Inst& inst,
                 ir::BlockId* next,
                 Outcome* outcome) {
    const ir::Function& function = *frame->function;
    switch (inst.opcode) {
      case ir::Opcode::kReturn:
        return inst.operands_count == 0 ? Value::Void()
                                        : Operand(frame, inst, 0);

      case ir::Opcode::kJump: {
        const std::span<const ir::BlockId> targets =
            function.SuccessorsOf(inst);
        if (targets.empty()) {
          return Trap(outcome, TrapKind::kUnreachable, At(function, id),
                      "a jump with nowhere to go");
        }
        // The jump's operands become the target's parameters, which is what
        // a phi node would otherwise have to work out backwards.
        const std::span<const ir::InstId> params =
            function.ParamsOf(targets[0]);
        std::vector<Value> incoming;
        for (ir::InstId operand : function.OperandsOf(inst)) {
          incoming.push_back(operand.index < frame->values.size()
                                 ? frame->values[operand.index]
                                 : Value());
        }
        for (size_t i = 0; i < params.size() && i < incoming.size(); ++i) {
          frame->values[params[i].index] = incoming[i];
        }
        *next = targets[0];
        return Value();
      }

      case ir::Opcode::kBranch: {
        const std::span<const ir::BlockId> targets =
            function.SuccessorsOf(inst);
        if (targets.size() != 2) {
          return Trap(outcome, TrapKind::kUnreachable, At(function, id),
                      "a branch without two targets");
        }
        *next = Operand(frame, inst, 0).boolean() ? targets[0] : targets[1];
        return Value();
      }

      case ir::Opcode::kSwitch: {
        const std::span<const ir::BlockId> targets =
            function.SuccessorsOf(inst);
        const std::span<const uint64_t> values = function.CasesOf(inst);
        if (targets.empty()) {
          return Trap(outcome, TrapKind::kUnreachable, At(function, id),
                      "a switch with nowhere to go");
        }
        const uint64_t subject = Operand(frame, inst, 0).integer();
        for (size_t i = 0; i < values.size() && i < targets.size(); ++i) {
          if (values[i] == subject) {
            *next = targets[i];
            return Value();
          }
        }
        *next = targets.back();
        return Value();
      }

      case ir::Opcode::kUnreachable:
        return Trap(outcome, TrapKind::kUnreachable, At(function, id),
                    "reached a point the compiler proved unreachable");

      default:
        return Trap(outcome, TrapKind::kUnreachable, At(function, id),
                    "not a terminator");
    }
  }

  // ------------------------------------------------------- type helpers --

  static uint32_t BitsOfWidth(IntWidth width) {
    switch (width) {
      case IntWidth::k8:
        return 8;
      case IntWidth::k16:
        return 16;
      case IntWidth::k32:
        return 32;
      case IntWidth::k64:
        return 64;
    }
    return 32;
  }

  static bool FitsSigned(int64_t value, IntWidth width) {
    const uint32_t bits = BitsOfWidth(width);
    if (bits >= 64) {
      return true;
    }
    const int64_t limit = int64_t{1} << (bits - 1);
    return value >= -limit && value < limit;
  }

  // Whether |source| survives a conversion to |width| and |is_signed|
  // unchanged. Both ends of the range matter: `-1 as u8` has no answer any
  // more than `300 as u8` does.
  static bool FitsAsInt(const Value& source, IntWidth width, bool is_signed) {
    const uint32_t bits = BitsOfWidth(width);
    if (source.is_signed()) {
      const int64_t value = source.signed_integer();
      if (is_signed) {
        return FitsSigned(value, width);
      }
      if (value < 0) {
        return false;
      }
      return bits >= 64 ||
             static_cast<uint64_t>(value) < (uint64_t{1} << bits);
    }
    const uint64_t value = source.integer();
    if (!is_signed) {
      return bits >= 64 || value < (uint64_t{1} << bits);
    }
    if (bits >= 64) {
      return value <= static_cast<uint64_t>(INT64_MAX);
    }
    return value < (uint64_t{1} << (bits - 1));
  }

  // Whether a truncated float lands inside an integer type. Written with
  // powers of two so the comparison is exact at every width, including the
  // 64-bit one where the limit is not representable as an integer.
  static bool FitsAsInteger(double truncated,
                            IntWidth width,
                            bool is_signed) {
    const uint32_t bits = BitsOfWidth(width);
    if (is_signed) {
      const double limit = std::ldexp(1.0, static_cast<int>(bits) - 1);
      return truncated >= -limit && truncated < limit;
    }
    const double limit = std::ldexp(1.0, static_cast<int>(bits));
    return truncated >= 0.0 && truncated < limit;
  }

  IntWidth WidthOf(TypeId type) const {
    const Type& described = types_.Get(type);
    return described.kind == TypeKind::kInt ? described.int_width
                                            : IntWidth::k32;
  }
  bool SignednessOf(TypeId type) const {
    const Type& described = types_.Get(type);
    return described.kind != TypeKind::kInt || described.is_signed;
  }
  FloatWidth FloatWidthOf(TypeId type) const {
    const Type& described = types_.Get(type);
    return described.kind == TypeKind::kFloat ? described.float_width
                                              : FloatWidth::k32;
  }

  const StringInterner& interner_;
  const TypeStore& types_;
  const ir::Module& module_;
  Limits limits_;
  Output* output_ = nullptr;
  ForeignCaller* foreign_ = nullptr;
  uint64_t steps_ = 0;
  uint32_t depth_ = 0;
};

}  // namespace

Outcome Run(const StringInterner& interner,
            const TypeStore& types,
            const ir::Module& module,
            const ir::Function& function,
            std::vector<Value> arguments,
            const Limits& limits,
            Output* output,
            ForeignCaller* foreign) {
  Interpreter interpreter(interner, types, module, limits, output, foreign);
  return interpreter.Run(function, std::move(arguments));
}

Outcome RunNamed(const StringInterner& interner,
                 const TypeStore& types,
                 const ir::Module& module,
                 Symbol name,
                 std::vector<Value> arguments,
                 const Limits& limits,
                 Output* output,
                 ForeignCaller* foreign) {
  const ir::Function* function = module.Find(name);
  if (function == nullptr) {
    Outcome outcome;
    outcome.trap = TrapKind::kNoBody;
    outcome.where = "@" + std::string(interner.Text(name));
    outcome.message = "is not in the module";
    return outcome;
  }
  return Run(interner, types, module, *function, std::move(arguments),
             limits, output, foreign);
}

}  // namespace interp
}  // namespace wolang
