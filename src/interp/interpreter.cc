// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/interp/interpreter.h"

#include "wolang/interp/marshal.h"

#include "wolang/runtime/globals.h"
#include "wolang/runtime/scheduler.h"

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
  // Which module the function came from: a cross-file call lands in
  // another one, and a string literal is an index into its own module.
  const ir::Module* module = nullptr;
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
              ForeignCaller* foreign,
              abi::LayoutStore* layouts,
              std::span<const ir::Module* const> linked)
      : interner_(interner),
        types_(types),
        module_(module),
        limits_(limits),
        output_(output),
        foreign_(foreign),
        layouts_(layouts),
        linked_(linked) {}

  Outcome Run(const ir::Function& function, std::vector<Value> arguments) {
    Outcome outcome;
    const Value result =
        Call(function, &module_, std::move(arguments), &outcome);
    if (outcome.ok()) {
      outcome.value = result;
    }
    return outcome;
  }

 private:
  Value Trap(Outcome* outcome,
             TrapKind kind,
             std::string where,
             std::string message,
             SourceSpan span = SourceSpan()) {
    if (outcome->ok()) {
      outcome->trap = kind;
      outcome->where = std::move(where);
      outcome->message = std::move(message);
      outcome->span = span;
    }
    return Value();
  }

  // The same, for the common case: the instruction says where it is and
  // the IR says where that came from (D118).
  Value TrapAt(Outcome* outcome,
               TrapKind kind,
               const ir::Function& function,
               ir::InstId id,
               std::string message) {
    return Trap(outcome, kind, At(function, id), std::move(message),
                function.SpanOf(id));
  }

  std::string At(const ir::Function& function, ir::InstId id) const {
    std::string result = "@";
    result.append(interner_.Text(function.name));
    result.append(":%");
    result.append(std::to_string(id.index));
    return result;
  }

  // |caller| is the frame the arguments came from, when there is one. Only
  // a foreign call needs it: a buffer in the caller's slot has to be
  // copied out to real memory, and copied back when the call returns.
  Value Call(const ir::Function& function,
             const ir::Module* home,
             std::vector<Value> arguments,
             Outcome* outcome,
             Frame* caller = nullptr) {
    if (function.is_foreign()) {
      return CallForeign(function, arguments, outcome, caller);
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
    frame.module = home;
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

  // Reading and writing through a raw pointer. Only a scalar: an aggregate
  // needs the layout the code generator computes, and the interpreter has
  // no reason to grow one until something needs it.
  // A slice and a `str` are a pointer and a length: two words, and
  // neither is a member anyone declared, so the layout cannot name their
  // types and this does.
  static bool IsPointerAndLength(TypeKind kind) {
    return kind == TypeKind::kSlice || kind == TypeKind::kStr;
  }

  Value LoadThroughPointer(const ir::Function& function,
                           const Value& pointer,
                           TypeId type,
                           ir::InstId id,
                           Outcome* outcome) {
    const Type& described = types_.Get(type);
    const auto address = reinterpret_cast<const void*>(pointer.integer());
    if (address == nullptr) {
      return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "read through a null pointer");
    }
    switch (described.kind) {
      case TypeKind::kBool: {
        uint8_t raw = 0;
        std::memcpy(&raw, address, sizeof(raw));
        return Value::Bool(raw != 0);
      }
      case TypeKind::kFloat: {
        if (described.float_width == FloatWidth::k64) {
          double number = 0;
          std::memcpy(&number, address, sizeof(number));
          return Value::Float(number, FloatWidth::k64);
        }
        float number = 0;
        std::memcpy(&number, address, sizeof(number));
        return Value::Float(static_cast<double>(number), FloatWidth::k32);
      }
      case TypeKind::kPointer: {
        uint64_t raw = 0;
        std::memcpy(&raw, address, sizeof(raw));
        return Value::Int(raw, IntWidth::k64, /*is_signed=*/false);
      }
      // A *function* is two words -- the code and what it closed over --
      // so it is read by the aggregate path below. Reading it as one
      // word was right while a function value was one, and became an
      // extract out of bounds when it stopped being (D138).
      case TypeKind::kInt: {
        uint64_t raw = 0;
        std::memcpy(&raw, address, WidthInBytes(described.int_width));
        return Value::Int(raw, described.int_width, described.is_signed);
      }
      default:
        break;
    }

    // An enum with no payload: a number in memory, and a one-field
    // aggregate in this tier -- which is the shape `extract 0` reads and
    // the shape a local one has. Reading it back as a bare number left a
    // `match` on a global `Level` trapping in the interpreter while the
    // compiled tiers answered (D120).
    if (layouts_ != nullptr && layouts_->IsSupported(type) &&
        layouts_->IsTagOnly(type)) {
      uint32_t which = 0;
      std::memcpy(&which, address,
                  std::min<size_t>(std::max(layouts_->SizeOf(type), 1u),
                                   sizeof(which)));
      std::vector<Value> one;
      one.push_back(Value::Int(which, IntWidth::k32, /*is_signed=*/false));
      return Value::Aggregate(std::move(one));
    }

    // A class handle, a reference, an error tag: everything whose value
    // is a number of its own size, including the ones whose *kind* is a
    // named type rather than an integer.
    if (layouts_ != nullptr && layouts_->IsSupported(type) &&
        layouts_->ClassOf(type) == abi::ValueClass::kInteger) {
      const uint32_t size = std::max(layouts_->SizeOf(type), 1u);
      uint64_t raw = 0;
      std::memcpy(&raw, address, std::min<size_t>(size, sizeof(raw)));
      return Value::Int(raw, IntWidth::k64, /*is_signed=*/false);
    }

    // An enum that carries values, read back: the tag says which variant
    // it is, and that variant's values are at the offsets the layout gave
    // them (D111).
    if (layouts_ != nullptr && layouts_->IsSupported(type) &&
        layouts_->IsTagged(type)) {
      uint32_t which = 0;
      std::memcpy(&which, address, sizeof(which));
      std::vector<Value> carried;
      for (uint32_t position = 0;; ++position) {
        const TypeId field = layouts_->VariantFieldType(type, which,
                                                        position);
        if (!field.is_valid()) {
          break;
        }
        const uint64_t at =
            pointer.integer() +
            layouts_->VariantFieldOffset(type, which, position);
        carried.push_back(LoadThroughPointer(
            function, Value::Int(at, IntWidth::k64, /*is_signed=*/false),
            field, id, outcome));
        if (!outcome->ok()) {
          return carried.back();
        }
      }
      std::vector<Value> pair;
      pair.push_back(Value::Int(which, IntWidth::k32,
                                /*is_signed=*/false));
      pair.push_back(Value::Aggregate(std::move(carried)));
      return Value::Aggregate(std::move(pair));
    }

    // The same, read back: an aggregate is its fields at their offsets.
    if (layouts_ != nullptr && layouts_->IsSupported(type) &&
        layouts_->ClassOf(type) == abi::ValueClass::kAggregate) {
      const abi::Layout& layout = layouts_->Of(type);
      std::vector<Value> elements;
      elements.reserve(layout.offsets.size());
      for (size_t i = 0; i < layout.offsets.size(); ++i) {
        const uint64_t at = pointer.integer() + layout.offsets[i];
        if (IsPointerAndLength(described.kind)) {
          uint64_t raw = 0;
          std::memcpy(&raw, reinterpret_cast<const void*>(at), sizeof(raw));
          elements.push_back(
              Value::Int(raw, IntWidth::k64, /*is_signed=*/false));
          continue;
        }
        const TypeId field =
            layouts_->FieldType(type, static_cast<uint32_t>(i));
        if (!field.is_valid()) {
          break;
        }
        elements.push_back(LoadThroughPointer(
            function, Value::Int(at, IntWidth::k64, /*is_signed=*/false),
            field, id, outcome));
        if (!outcome->ok()) {
          return elements.back();
        }
      }
      return Value::Aggregate(std::move(elements));
    }
    return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "read a type through a pointer that this tier cannot "
                "lay out: " +
                    types_.Describe(interner_, type));
  }

  Value StoreThroughPointer(const ir::Function& function,
                            const Value& pointer,
                            const Value& value,
                            ir::InstId id,
                            Outcome* outcome,
                            TypeId type = TypeId{}) {
    const auto address = reinterpret_cast<void*>(pointer.integer());
    if (address == nullptr) {
      return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "wrote through a null pointer");
    }
    // A struct, a slice, a tuple: written field by field at the offsets
    // the layout gives, which is the same answer the compiled tiers get
    // from the same store. A class with a slice in it is the ordinary case
    // -- it was only ever unsupported because nothing had one yet.
    // An enum that carries values: the tag, and the payload at the
    // offsets the layout gave *that variant*. Not the field offsets of an
    // aggregate -- every variant's values start at the same place, which
    // is what makes one read site work for all of them (D89) and what the
    // plain field loop below cannot express (D111).
    if (value.kind() == Value::Kind::kAggregate && layouts_ != nullptr &&
        type.is_valid() && layouts_->IsTagged(type) &&
        value.elements().size() == 2) {
      const std::span<const Value> pair = value.elements();
      const Value tag = Value::Int(pair[0].integer(), IntWidth::k32,
                                   /*is_signed=*/false);
      const Value written =
          StoreThroughPointer(function, pointer, tag, id, outcome);
      if (!outcome->ok()) {
        return written;
      }
      const auto which = static_cast<uint32_t>(pair[0].integer());
      const std::span<const Value> carried = pair[1].elements();
      for (size_t i = 0; i < carried.size(); ++i) {
        const Value at = Value::Int(
            pointer.integer() +
                layouts_->VariantFieldOffset(type, which,
                                             static_cast<uint32_t>(i)),
            IntWidth::k64, /*is_signed=*/false);
        const Value one = StoreThroughPointer(
            function, at, carried[i], id, outcome,
            layouts_->VariantFieldType(type, which,
                                       static_cast<uint32_t>(i)));
        if (!outcome->ok()) {
          return one;
        }
      }
      return Value::Void();
    }
    if (value.kind() == Value::Kind::kAggregate && type.is_valid() &&
        layouts_ != nullptr && layouts_->IsSupported(type) &&
        layouts_->IsTagOnly(type)) {
      // The other half of the pair above: one field, which is the tag.
      const std::span<const Value> elements = value.elements();
      const uint32_t which = elements.empty()
                                 ? 0
                                 : static_cast<uint32_t>(
                                       elements[0].integer());
      const size_t size = std::min<size_t>(
          std::max(layouts_->SizeOf(type), 1u), sizeof(which));
      std::memcpy(address, &which, size);
      return Value::Void();
    }
    if (value.kind() == Value::Kind::kAggregate && layouts_ != nullptr &&
        type.is_valid()) {
      const abi::Layout& layout = layouts_->Of(type);
      const std::span<const Value> elements = value.elements();
      for (size_t i = 0; i < elements.size() && i < layout.offsets.size();
           ++i) {
        const Value at = Value::Int(
            pointer.integer() + layout.offsets[i], IntWidth::k64,
            /*is_signed=*/false);
        const TypeId field =
            IsPointerAndLength(types_.KindOf(type))
                ? TypeId{}
                : layouts_->FieldType(type, static_cast<uint32_t>(i));
        const Value written = StoreThroughPointer(function, at, elements[i],
                                                  id, outcome, field);
        if (!outcome->ok()) {
          return written;
        }
      }
      return Value::Void();
    }
    switch (value.kind()) {
      case Value::Kind::kBool: {
        const uint8_t raw = value.boolean() ? 1 : 0;
        std::memcpy(address, &raw, sizeof(raw));
        return Value::Void();
      }
      case Value::Kind::kFloat: {
        if (value.float_width() == FloatWidth::k64) {
          const double number = value.floating();
          std::memcpy(address, &number, sizeof(number));
        } else {
          const auto number = static_cast<float>(value.floating());
          std::memcpy(address, &number, sizeof(number));
        }
        return Value::Void();
      }
      case Value::Kind::kInt: {
        const uint64_t raw = value.integer();
        std::memcpy(address, &raw, WidthInBytes(value.int_width()));
        return Value::Void();
      }
      case Value::Kind::kFunction: {
        // A function value has no address in this tier -- it is a module
        // and an index -- so what goes into memory is the token a call
        // through it turns back into the value. The same token the
        // compiled tier's address would be.
        const uint64_t token = Token(value);
        std::memcpy(address, &token, sizeof(token));
        return Value::Void();
      }
      default:
        return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "wrote a type through a pointer that this tier cannot "
                    "lay out: " +
                        value.Describe());
    }
  }

  static size_t WidthInBytes(IntWidth width) {
    switch (width) {
      case IntWidth::k8:
        return 1;
      case IntWidth::k16:
        return 2;
      case IntWidth::k32:
        return 4;
      case IntWidth::k64:
        return 8;
    }
    return 4;
  }

  // Out of the language, through the thunk the host handed over. The
  // arguments are reduced to bits and a register file, which is all the C
  // ABI needs; the checker already made sure every type at the boundary has
  // a representation on both sides (`E0802`).
  Value CallForeign(const ir::Function& function,
                    const std::vector<Value>& arguments,
                    Outcome* outcome,
                    Frame* caller) {
    const std::string symbol(interner_.Text(function.foreign_symbol));

    // What was copied out to real memory for the call, so that it can be
    // copied back when the call is over: a `read` into a buffer is the
    // whole reason this exists.
    std::vector<Bounced> bounced;
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
      } else if (argument.kind() == Value::Kind::kFunction) {
        // A function value has no address here -- it is a module and an
        // index -- so what crosses the boundary is a token this run can
        // turn back into the value. Which is what lets a signal keep its
        // handlers in the same list the compiled tier uses.
        entry.bits = Token(argument);
      } else if (argument.kind() == Value::Kind::kSlot ||
                 argument.kind() == Value::Kind::kPlace) {
        // A buffer in a frame slot, handed to something outside the
        // language. The interpreter's values carry their shape rather
        // than living in memory, so there is no address to pass -- the
        // bytes are copied out to one, and copied back afterwards.
        //
        // That is the honest thing a tier of this shape can do, and it is
        // what lets a program that reads into a buffer run interpreted as
        // well as compiled.
        if (caller == nullptr || !Bounce(caller, argument, &bounced)) {
          return Trap(outcome, TrapKind::kUndefinedValue, "@" + symbol,
                      "was given " + argument.Describe() +
                          ", which has no representation in C");
        }
        entry.bits = reinterpret_cast<uint64_t>(
            bounced.back().bytes.data() + bounced.back().offset);
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

    // The runtime's own functions are called directly rather than through
    // the C boundary: building a string must not depend on there being a
    // thunk, because the reference tier has to work on a machine with no
    // backend at all. They get the same arguments the boundary would --
    // addresses, with a buffer already copied out to real memory.
    // A stage of a schedule, in a tier whose values are not addresses:
    // the systems are remembered as the values they are and run one after
    // another (D101). A stage is a set the compiler proved independent,
    // so running them in a row gives what running them together gives.
    if (symbol == "wolang_stage_add" && !arguments.empty() &&
        arguments[0].kind() == Value::Kind::kFunction) {
      stage_.push_back(arguments[0]);
      return Value();
    }
    if (symbol == "wolang_stage_run") {
      std::vector<Value> stage;
      stage.swap(stage_);
      for (const Value& system : stage) {
        const auto* home =
            static_cast<const ir::Module*>(system.module());
        if (home == nullptr || system.function() >= home->functions.size()) {
          continue;
        }
        std::vector<Value> given;
        if (arguments.size() >= 2) {
          given.push_back(arguments[0]);
          given.push_back(arguments[1]);
        }
        Outcome inner;
        const ir::Function* system_function =
            &home->functions[system.function()];
        const ir::Module* where = home;
        Link(&system_function, &where);
        Call(*system_function, where, std::move(given), &inner, caller);
        if (!inner.ok()) {
          *outcome = inner;
          return Value();
        }
      }
      return Value();
    }

    // A thread, in a tier whose values are not addresses: the body is
    // run here and now, on this thread (D97). A program whose answer does
    // not depend on interleaving gives the same answer either way, which
    // is the only kind the differential check can compare -- and a
    // program that *does* depend on it has a bug the comparison should
    // not be hiding.
    if (symbol == "wolang_thread_spawn" && !arguments.empty() &&
        arguments[0].kind() == Value::Kind::kFunction) {
      Outcome inner;
      const Value body = arguments[0];
      const auto* home = static_cast<const ir::Module*>(body.module());
      if (home != nullptr && body.function() < home->functions.size()) {
        Call(home->functions[body.function()], home, {}, &inner, caller);
      }
      if (!inner.ok()) {
        *outcome = inner;
        return Value();
      }
      // A handle that is already finished, which is what `Join` on it
      // means here.
      return Value::Int(1, IntWidth::k64, /*is_signed=*/false);
    }
    if (symbol == "wolang_thread_join") {
      return Value::Int(0, IntWidth::k32, /*is_signed=*/true);
    }

    const Intrinsic builtin = LookupIntrinsic(symbol);
    if (builtin != Intrinsic::kNone) {
      std::vector<Value> direct;
      direct.reserve(raw.size());
      for (size_t i = 0; i < raw.size(); ++i) {
        if (i < arguments.size() &&
            arguments[i].kind() != Value::Kind::kSlot &&
            arguments[i].kind() != Value::Kind::kPlace) {
          direct.push_back(arguments[i]);
          continue;
        }
        direct.push_back(Value::Int(raw[i].bits, IntWidth::k64,
                                    /*is_signed=*/false));
      }
      Value result = CallIntrinsic(builtin, direct, output_);
      if (!result.is_undefined()) {
        CopyBack(&bounced);
        return result;
      }
    }

    if (foreign_ == nullptr) {
      // The runtime's own functions were handled above; anything else
      // needs a way out of the language, and this run has none.
      return Trap(outcome, TrapKind::kNoBody, "@" + symbol,
                  "is `extern`, and this run cannot call out of the "
                  "language");
    }
    uint64_t result = 0;
    switch (foreign_->Call(symbol, raw, returns_float, &result)) {
      case ForeignOutcome::kCalled:
        break;
      case ForeignOutcome::kNoSymbol:
        return Trap(outcome, TrapKind::kNoBody, "@" + symbol,
                    "is `extern`, and its symbol was not found");
      case ForeignOutcome::kTooManyArguments:
        return Trap(outcome, TrapKind::kNoBody, "@" + symbol,
                    "is `extern`, and takes more arguments than a call "
                    "out of the language can pass");
    }
    CopyBack(&bounced);

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

  // The declared type of an operand, which an instruction that works on
  // addresses needs: the value itself is just an integer.
  static TypeId TypeOf(const ir::Function& function,
                       const ir::Inst& inst,
                       size_t index) {
    const std::span<const ir::InstId> operands = function.OperandsOf(inst);
    return index < operands.size() && function.Contains(operands[index])
               ? function.Get(operands[index]).type
               : TypeId{};
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
      case ir::Opcode::kConstString: {
        // A string is a pointer and a length, here as much as in machine
        // code: the bytes live with the module, so a literal is a view of
        // them. One representation in every tier is what lets the
        // standard library build a string rather than the compiler.
        static const std::string kEmpty;
        const std::string& bytes =
            inst.payload.string < frame->module->strings.size()
                ? frame->module->strings[inst.payload.string]
                : kEmpty;
        std::vector<Value> pair;
        pair.push_back(Value::Int(reinterpret_cast<uint64_t>(bytes.data()),
                                  IntWidth::k64, /*is_signed=*/false));
        pair.push_back(Value::Int(bytes.size(), IntWidth::k64,
                                  /*is_signed=*/false));
        return Value::Aggregate(std::move(pair));
      }

      case ir::Opcode::kUndef:
        // Reaching one means control went where the front end proved it
        // could not, so it is a trap rather than a zero.
        return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "an undefined value was used");

      case ir::Opcode::kAlloca: {
        // Zeroed, the way a slot is in the compiled tiers. The two have to
        // agree about a slot that was only partly written -- one element
        // of a buffer, one field of a struct -- and `E0123` is what
        // catches reading one that was never written at all.
        frame->slots.push_back(ZeroOf(inst.type));
        return Value::Slot(
            static_cast<uint32_t>(frame->slots.size() - 1), inst.type);
      }
      case ir::Opcode::kFuncAddress:
        // Which function, and which module it belongs to: an index alone
        // would mean a different function in another file.
        return Value::Function(inst.payload.callee, frame->module);

      case ir::Opcode::kNew: {
        // A class is a handle, and this is what it is a handle to. The
        // address is a real one, so reading and writing a field of an
        // object is the same code a raw pointer goes through -- which is
        // why a method of a class mutates the object and a method of a
        // struct cannot.
        if (layouts_ == nullptr) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "an object needs a layout to be allocated");
        }
        // A class's object, or the value a box holds.
        const TypeId allocated =
            types_.KindOf(inst.type) == TypeKind::kPointer
                ? types_.ElementOf(inst.type)
                : inst.type;
        void* object = runtime::wolang_object_alloc(
            layouts_->ObjectOf(allocated).size);
        if (object == nullptr) {
          return TrapAt(outcome, TrapKind::kOutOfMemory, function, id,
                "out of memory");
        }
        return Value::Int(reinterpret_cast<uint64_t>(object), IntWidth::k64,
                          /*is_signed=*/false);
      }
      case ir::Opcode::kMakeVariant: {
        // The tag, and what the variant carries beside it. This tier
        // keeps the payload as the values themselves rather than as
        // bytes, which is the same latitude it takes with every
        // aggregate (D89).
        std::vector<Value> carried;
        for (ir::InstId operand : function.OperandsOf(inst)) {
          carried.push_back(operand.index < frame->values.size()
                                ? frame->values[operand.index]
                                : Value());
        }
        std::vector<Value> pair;
        pair.push_back(Value::Int(inst.field, IntWidth::k32,
                                  /*is_signed=*/false));
        pair.push_back(Value::Aggregate(std::move(carried)));
        return Value::Aggregate(std::move(pair));
      }

      case ir::Opcode::kVariantPayload: {
        const Value& subject = Operand(frame, inst, 0);
        if (subject.kind() != Value::Kind::kAggregate ||
            subject.elements().size() < 2) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "read a variant's payload out of something that is "
                      "not a variant");
        }
        const Value& carried = subject.elements()[1];
        const auto position =
            static_cast<size_t>(inst.payload.integer);
        if (carried.kind() != Value::Kind::kAggregate ||
            position >= carried.elements().size()) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "read a value the variant does not carry");
        }
        return carried.elements()[position];
      }

      case ir::Opcode::kLocalAddress: {
        // A slot *is* where the value is in this tier: there are no
        // frame addresses here, and a place is a path (D94). What crosses
        // the C boundary is bounced into real memory by the marshaller.
        //
        // As a *place* rather than as the bare slot, because a slot is an
        // index into the frame that produced it: handing one to a callee
        // -- which is what `&mut x` is for -- made the index mean a slot
        // of the callee's own frame (D111). A place names its frame.
        const Value& slot = Operand(frame, inst, 0);
        if (slot.kind() == Value::Kind::kSlot) {
          // The type is what the address points *at*: the place carries
          // it so that marshalling across the C boundary copies the whole
          // value out and not a pointer's worth of it.
          return Value::Place(frame, slot.slot(), {},
                              types_.ElementOf(inst.type));
        }
        return slot;
      }

      case ir::Opcode::kGlobalAddress: {
        // The runtime owns the storage, so the address is the same one
        // compiled code would get (D88).
        const TypeId held = types_.ElementOf(inst.type);
        const uint64_t size =
            layouts_ != nullptr && held.is_valid()
                ? layouts_->SizeOf(held)
                : 8;
        void* storage = runtime::wolang_global_address(
            inst.payload.integer, size == 0 ? 8 : size);
        return Value::Int(reinterpret_cast<uint64_t>(storage),
                          IntWidth::k64, /*is_signed=*/false);
      }

      case ir::Opcode::kClassTable: {
        // Which declaration this class is. The same number in every tier,
        // which is what lets an object made here be dispatched in
        // compiled code and the other way round.
        const uint32_t index = inst.payload.klass;
        const uint64_t token =
            index < frame->module->classes.size()
                ? frame->module->classes[index].token
                : 0;
        return Value::Int(token, IntWidth::k64, /*is_signed=*/false);
      }

      case ir::Opcode::kVirtualCall: {
        // The token, then the receiver, then the arguments: one shape
        // whether the receiver came from a class handle or out of an ability
        // value's pair.
        const Value& token_value = Operand(frame, inst, 0);
        if (token_value.kind() != Value::Kind::kInt) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "a virtual call needs a class token");
        }
        const uint64_t token = token_value.integer();
        const ir::Module* home = nullptr;
        const ir::ClassInfo* klass = FindClass(frame->module, token, &home);
        uint32_t callee = 0xFFFFFFFFu;
        if (klass != nullptr) {
          for (const ir::ClassInfo::Method& method : klass->methods) {
            if (method.selector == inst.payload.integer) {
              callee = method.function;
              break;
            }
          }
        }
        if (callee == 0xFFFFFFFFu) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "a virtual call found no body for its method");
        }
        if (callee >= home->functions.size()) {
          return TrapAt(outcome, TrapKind::kNoBody, function, id,
                "a virtual call names a function the module does not "
                      "have");
        }
        std::vector<Value> arguments;
        const std::span<const ir::InstId> operands =
            function.OperandsOf(inst);
        for (size_t i = 1; i < operands.size(); ++i) {
          const ir::InstId operand = operands[i];
          arguments.push_back(operand.index < frame->values.size()
                                  ? frame->values[operand.index]
                                  : Value());
        }
        return Call(home->functions[callee], home, std::move(arguments),
                    outcome, frame);
      }

      case ir::Opcode::kLoad: {
        const Value& slot = Operand(frame, inst, 0);
        if (slot.kind() == Value::Kind::kPlace) {
          const Value* at = Read(frame, slot);
          if (at == nullptr || at->is_undefined()) {
            return Trap(outcome, TrapKind::kUndefinedValue,
                        At(function, id),
                        "read a part of a slot before anything was "
                        "written to it");
          }
          return *at;
        }
        if (slot.kind() == Value::Kind::kInt) {
          // A raw pointer. Reading through one is `unsafe` and the checker
          // said so; here it means what it says, and the interpreter reads
          // the memory the program asked for.
          return LoadThroughPointer(function, slot, inst.type, id,
                                    outcome);
        }
        if (slot.kind() != Value::Kind::kSlot ||
            slot.slot() >= frame->slots.size()) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "loaded from something that is not a slot");
        }
        const Value& stored = frame->slots[slot.slot()];
        if (stored.is_undefined()) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "read a slot before anything was written to it");
        }
        return stored;
      }
      case ir::Opcode::kStore: {
        const Value& slot = Operand(frame, inst, 0);
        if (slot.kind() == Value::Kind::kPlace) {
          Value* at = Walk(frame, slot);
          if (at == nullptr) {
            return TrapAt(outcome, TrapKind::kOutOfBounds, function, id,
                "wrote outside the slot");
          }
          *at = Operand(frame, inst, 1);
          return Value::Void();
        }
        if (slot.kind() == Value::Kind::kInt) {
          // The type written is the one the pointer points at, which is
          // what an aggregate needs in order to find its fields.
          const ir::InstId target = function.OperandsOf(inst)[0];
          const TypeId pointer = function.Contains(target)
                                     ? function.Get(target).type
                                     : TypeId{};
          TypeId written =
              pointer.is_valid() &&
                      types_.KindOf(pointer) == TypeKind::kPointer
                  ? types_.ElementOf(pointer)
                  : TypeId{};
          // The value's own type when the address does not say: an
          // element address inside `soa` storage is typed as the field it
          // points at, and a store of a whole record through one left the
          // interpreter with nothing to lay the record out by (D111).
          if (!written.is_valid()) {
            const ir::InstId source = function.OperandsOf(inst)[1];
            written = function.Contains(source) ? function.Get(source).type
                                                : TypeId{};
          }
          return StoreThroughPointer(function, slot,
                                     Operand(frame, inst, 1), id, outcome,
                                     written);
        }
        if (slot.kind() != Value::Kind::kSlot ||
            slot.slot() >= frame->slots.size()) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
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
        if (aggregate.kind() == Value::Kind::kString) {
          // A string's parts: the bytes it holds, and how many. The pointer
          // is into the value's own storage, which lives as long as the
          // value does -- the same guarantee a `str` makes.
          if (inst.field == 1) {
            return Value::Int(aggregate.text().size(), IntWidth::k64,
                              /*is_signed=*/false);
          }
          return Value::Int(
              reinterpret_cast<uint64_t>(aggregate.text().data()),
              IntWidth::k64, /*is_signed=*/false);
        }
        if (aggregate.kind() != Value::Kind::kAggregate ||
            inst.field >= aggregate.elements().size()) {
          return TrapAt(outcome, TrapKind::kOutOfBounds, function, id,
                "field " + std::to_string(inst.field) +
                          " is not there");
        }
        return aggregate.elements()[inst.field];
      }
      case ir::Opcode::kInsert: {
        const Value& aggregate = Operand(frame, inst, 0);
        if (aggregate.kind() != Value::Kind::kAggregate ||
            inst.field >= aggregate.elements().size()) {
          return TrapAt(outcome, TrapKind::kOutOfBounds, function, id,
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
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "indexed something that is not an array");
        }
        if (index.integer() >= array.elements().size()) {
          // The message says what both tiers know. The offending index is
          // a runtime value, and a code generator cannot put it in a
          // string it built while compiling -- so keeping it out is what
          // makes the two tiers' outcomes comparable in full.
          return TrapAt(outcome, TrapKind::kOutOfBounds, function, id,
                "index is outside a length of " +
                          std::to_string(array.elements().size()));
        }
        return array.elements()[index.integer()];
      }

      case ir::Opcode::kTypeSize: {
        // The tier's own answer about its own layout (D100).
        const TypeId measured{static_cast<uint32_t>(inst.payload.integer)};
        if (layouts_ == nullptr || !measured.is_valid()) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "asked how big a type is without a layout to ask");
        }
        return Value::Int(std::max(layouts_->SizeOf(measured), 1u),
                          IntWidth::k64, /*is_signed=*/true);
      }

      case ir::Opcode::kSoaNew: {
        // Storage for a record, one array per field. The size is this
        // tier's answer, the way `new`'s is.
        if (layouts_ == nullptr) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "storage needs a layout to be allocated");
        }
        const TypeId record = types_.ElementOf(inst.type);
        const uint64_t count = Operand(frame, inst, 0).integer();
        // One array per field, or one record after another: the layout the
        // result type asks for.
        const uint64_t bytes =
            types_.Get(inst.type).is_soa
                ? layouts_->SoaSizeOf(record, count)
                : count * layouts_->StrideOf(inst.type);
        void* block = runtime::wolang_object_alloc(bytes);
        if (block == nullptr) {
          return TrapAt(outcome, TrapKind::kOutOfMemory, function, id,
                "out of memory");
        }
        std::vector<Value> pair;
        pair.push_back(Value::Int(reinterpret_cast<uint64_t>(block),
                                  IntWidth::k64, /*is_signed=*/false));
        pair.push_back(Value::Int(count, IntWidth::k64,
                                  /*is_signed=*/false));
        return Value::Aggregate(std::move(pair));
      }

      case ir::Opcode::kIndexCheck: {
        // All that is left where an element address was expanded into
        // arithmetic: the element still has to exist.
        const uint64_t index = Operand(frame, inst, 0).integer();
        const uint64_t length = Operand(frame, inst, 1).integer();
        if (index >= length) {
          return TrapAt(outcome, TrapKind::kOutOfBounds, function, id,
                "index is outside a length of " +
                          std::to_string(length));
        }
        return Value();
      }

      case ir::Opcode::kSoaAddress: {
        const Value& base = Operand(frame, inst, 0);
        const Value& index = Operand(frame, inst, 1);
        const Value& count = Operand(frame, inst, 2);
        if (base.kind() != Value::Kind::kInt || layouts_ == nullptr) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "took a field address inside storage that is not at "
                      "an address");
        }
        if (index.integer() >= count.integer()) {
          return TrapAt(outcome, TrapKind::kOutOfBounds, function, id,
                "index is outside the length");
        }
        const TypeId record = SoaRecordOf(function, inst);
        return Value::Int(
            base.integer() + layouts_->SoaOffsetOf(record, inst.field,
                                                   index.integer(),
                                                   count.integer()),
            IntWidth::k64, /*is_signed=*/false);
      }

      case ir::Opcode::kIndexAddress: {
        // The element of an array at an address. Inside a slot it is a
        // place -- a path -- because the interpreter's values carry their
        // shape rather than living at addresses.
        const Value& base = Operand(frame, inst, 0);
        const Value& index = Operand(frame, inst, 1);
        // The length is an operand because a slice's length is a value.
        const uint64_t length = Operand(frame, inst, 2).integer();
        if (index.integer() >= length) {
          return TrapAt(outcome, TrapKind::kOutOfBounds, function, id,
                "index is outside a length of " +
                          std::to_string(length));
        }
        if (base.kind() == Value::Kind::kSlot ||
            base.kind() == Value::Kind::kPlace) {
          return Extend(frame, base,
                        static_cast<uint32_t>(index.integer()),
                        /*is_element=*/true,
                        AddressedType(function, inst, 0));
        }
        if (base.kind() != Value::Kind::kInt || layouts_ == nullptr) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "took an element address without an address to take "
                      "it from");
        }
        // The stride is the size of what the result points at: the base
        // is an array for an index into a slot and the element itself for
        // one into a slice.
        const uint64_t stride = layouts_->StrideOf(inst.type);
        return Value::Int(base.integer() + index.integer() * stride,
                          IntWidth::k64, /*is_signed=*/false);
      }

      case ir::Opcode::kFieldAddress: {
        // The address of a field, which is the layout's answer and nobody
        // else's -- the interpreter's own values carry their shape, so this
        // is the one question it cannot answer alone.
        const Value& base = Operand(frame, inst, 0);
        if (base.kind() == Value::Kind::kSlot ||
            base.kind() == Value::Kind::kPlace) {
          return Extend(frame, base, inst.field, /*is_element=*/false,
                        AddressedType(function, inst, 0));
        }
        if (base.kind() != Value::Kind::kInt || layouts_ == nullptr) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "took a field address without an address to take it "
                      "from");
        }
        // Through a pointer it is what the pointer points at; through a
        // class handle it is the class itself.
        const TypeId base_type = TypeOf(function, inst, 0);
        const TypeId aggregate = types_.IsHandle(base_type)
                                     ? base_type
                                     : types_.ElementOf(base_type);
        const abi::Layout& layout = layouts_->ObjectOf(aggregate);
        if (inst.field >= layout.offsets.size()) {
          // Offset zero was the old answer, and it wrote one field over
          // another: a layout that does not know the type is a fault in
          // this tier, not a guess (D125).
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "took the address of field " +
                    std::to_string(inst.field) + " of " +
                    types_.Describe(interner_, aggregate) +
                    ", which this tier cannot lay out");
        }
        const uint64_t offset = layout.offsets[inst.field];
        return Value::Int(base.integer() + offset, IntWidth::k64,
                          /*is_signed=*/false);
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
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "read the value of an optional that is absent");
        }
        return optional.inside();
      }

      case ir::Opcode::kCallIndirect: {
        Value target = Operand(frame, inst, 0);
        if (target.kind() == Value::Kind::kInt) {
          // A token that came back from the runtime: a signal's handler
          // list holds these, because a function value cannot be an
          // address in this tier.
          target = FromToken(target.integer());
        }
        if (target.kind() != Value::Kind::kFunction) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "called " + target.Describe() +
                          ", which is not a function");
        }
        const auto* home = static_cast<const ir::Module*>(target.module());
        if (home == nullptr || target.function() >= home->functions.size()) {
          return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "called a function that is not in its module");
        }
        std::vector<Value> arguments;
        const std::span<const ir::InstId> operands =
            function.OperandsOf(inst);
        for (size_t i = 1; i < operands.size(); ++i) {
          arguments.push_back(operands[i].index < frame->values.size()
                                  ? frame->values[operands[i].index]
                                  : Value());
        }
        // The value may name a declaration rather than a body: a
        // function passed from one file to another is one here and the
        // other there, and calling it has to find the body the same way a
        // direct call does.
        const ir::Function* callee = &home->functions[target.function()];
        Link(&callee, &home);
        return Call(*callee, home, std::move(arguments), outcome, frame);
      }

      case ir::Opcode::kCall: {
        // A parallel loop, run in one go: this tier is the reference, and
        // the reference answer is the sequential one. The body is a
        // function value in two halves -- the code as a token here -- and
        // what it gets is the whole range (D147).
        if (inst.payload.callee < frame->module->functions.size()) {
          const ir::Function& called =
              frame->module->functions[inst.payload.callee];
          if (called.is_foreign() &&
              interner_.Text(called.foreign_symbol) ==
                  "wolang_parallel_for") {
            const std::span<const ir::InstId> operands =
                function.OperandsOf(inst);
            if (operands.size() < 3) {
              return Value::Void();
            }
            Value body = Operand(frame, inst, 0);
            if (body.kind() == Value::Kind::kInt) {
              body = FromToken(body.integer());
            }
            if (body.kind() != Value::Kind::kFunction) {
              return Value::Void();
            }
            const Value environment = Operand(frame, inst, 1);
            const uint64_t count = Operand(frame, inst, 2).integer();
            const auto* home =
                static_cast<const ir::Module*>(body.module());
            if (home == nullptr ||
                body.function() >= home->functions.size()) {
              return Value::Void();
            }
            std::vector<Value> arguments;
            if (environment.integer() != 0) {
              arguments.push_back(environment);
            }
            arguments.push_back(Value::Int(0, IntWidth::k64,
                                           /*is_signed=*/true));
            arguments.push_back(
                Value::Int(count, IntWidth::k64, /*is_signed=*/true));
            const ir::Function* callee = &home->functions[body.function()];
            const ir::Module* where = home;
            Link(&callee, &where);
            return Call(*callee, where, std::move(arguments), outcome,
                        frame);
          }
        }
        if (inst.payload.callee >= frame->module->functions.size()) {
          return TrapAt(outcome, TrapKind::kNoBody, function, id,
                "calls a function the module does not have");
        }
        std::vector<Value> arguments;
        for (ir::InstId operand : function.OperandsOf(inst)) {
          arguments.push_back(operand.index < frame->values.size()
                                  ? frame->values[operand.index]
                                  : Value());
        }
        const ir::Function* callee =
            &frame->module->functions[inst.payload.callee];
        const ir::Module* home = frame->module;
        Link(&callee, &home);
        return Call(*callee, home, std::move(arguments), outcome, frame);
      }

      case ir::Opcode::kCast:
        return Convert(frame, id, inst, outcome);

      case ir::Opcode::kLogicalNot: {
        const Value& operand = Operand(frame, inst, 0);
        return Value::Bool(!operand.boolean());
      }

      default:
        if (inst.is_unchecked) {
          return Unchecked(frame, inst);
        }
        return Arithmetic(frame, id, inst, outcome);
    }
  }

  // A buffer copied out to real memory for a foreign call, and where it
  // came from.
  struct Bounced {
    Value* slot = nullptr;
    TypeId type;
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;
  };

  // A function value as something C can hold, and back.
  //
  // The runtime stores whatever it is given in a list of pointers; this
  // tier has no pointers to functions, so it hands out a token instead and
  // keeps the value. One table per run, which is as long as a signal's
  // handlers are reachable.
  uint64_t Token(const Value& function) {
    for (size_t i = 0; i < tokens_.size(); ++i) {
      if (tokens_[i] == function) {
        return i + 1;
      }
    }
    tokens_.push_back(function);
    return tokens_.size();
  }

  Value FromToken(uint64_t token) const {
    if (token == 0 || token > tokens_.size()) {
      return Value();
    }
    return tokens_[token - 1];
  }

  // Whatever a call wrote into a bounced buffer belongs back where it
  // came from.
  void CopyBack(std::vector<Bounced>* bounced) {
    for (Bounced& entry : *bounced) {
      if (entry.slot != nullptr) {
        *entry.slot =
            Decode(types_, layouts_, entry.bytes.data(), entry.type);
      }
    }
    bounced->clear();
  }

  // Copies what a place holds into real memory, and remembers where to
  // put it back. False when the place cannot be found or its type is not
  // known: without a layout there is nothing to copy.
  bool Bounce(Frame* frame, const Value& place, std::vector<Bounced>* out) {
    if (layouts_ == nullptr || !place.place_type().is_valid()) {
      return false;
    }
    // The root of the place, not the leaf: the whole slot travels.
    auto* home = const_cast<Frame*>(static_cast<const Frame*>(
        place.frame() != nullptr ? place.frame() : frame));
    const uint32_t index = place.kind() == Value::Kind::kPlace
                               ? static_cast<uint32_t>(place.integer())
                               : place.slot();
    Value* at = index < home->slots.size() ? &home->slots[index] : nullptr;
    if (at == nullptr) {
      return false;
    }
    Bounced entry;
    entry.slot = at;
    entry.type = place.place_type();
    const uint32_t size = layouts_->SizeOf(entry.type);
    if (size == 0) {
      return false;
    }
    entry.bytes.assign(size, 0);
    Encode(types_, layouts_, *at, entry.type, entry.bytes.data());
    entry.offset = OffsetOfPlace(place);
    if (entry.offset >= size) {
      return false;
    }
    out->push_back(std::move(entry));
    return true;
  }

  // Where in the slot's bytes a place points. The whole slot is copied out,
  // so the address handed to C is this far into it.
  uint32_t OffsetOfPlace(const Value& place) {
    if (place.kind() != Value::Kind::kPlace || layouts_ == nullptr) {
      return 0;
    }
    TypeId at = place.place_type();
    uint32_t offset = 0;
    for (const PlaceStep& step : place.path()) {
      if (step.is_element) {
        offset += step.index * layouts_->StrideOf(at);
        at = types_.ElementOf(at);
        continue;
      }
      const abi::Layout& layout = layouts_->ObjectOf(at);
      offset += step.index < layout.offsets.size()
                    ? layout.offsets[step.index]
                    : 0;
      at = layouts_->FieldType(at, step.index);
    }
    return offset;
  }

  // A value of |type| with every part zero. What a slot starts as.
  Value ZeroOf(TypeId type) {
    switch (types_.KindOf(type)) {
      case TypeKind::kBool:
        return Value::Bool(false);
      case TypeKind::kFloat:
        return Value::Float(0.0, types_.Get(type).float_width);
      case TypeKind::kInt:
        return Value::Int(0, types_.Get(type).int_width,
                          types_.Get(type).is_signed);
      case TypeKind::kChar:
        return Value::Int(0, IntWidth::k32, /*is_signed=*/false);
      case TypeKind::kPointer:
      case TypeKind::kNamed:
        if (types_.IsHandle(type)) {
          return Value::Int(0, IntWidth::k64, /*is_signed=*/false);
        }
        break;
      case TypeKind::kOptional:
        return Value::Absent();
      default:
        break;
    }
    if (types_.KindOf(type) == TypeKind::kPointer) {
      return Value::Int(0, IntWidth::k64, /*is_signed=*/false);
    }
    if (types_.KindOf(type) == TypeKind::kArray) {
      const TypeId element = types_.ElementOf(type);
      std::vector<Value> elements;
      elements.reserve(static_cast<size_t>(types_.Get(type).length));
      for (uint64_t i = 0; i < types_.Get(type).length; ++i) {
        elements.push_back(ZeroOf(element));
      }
      return Value::Aggregate(std::move(elements));
    }
    if (types_.KindOf(type) == TypeKind::kNamed ||
        types_.KindOf(type) == TypeKind::kTuple) {
      std::vector<Value> elements;
      if (layouts_ != nullptr) {
        for (uint32_t i = 0; i < layouts_->FieldCount(type); ++i) {
          elements.push_back(ZeroOf(layouts_->FieldType(type, i)));
        }
      }
      return Value::Aggregate(std::move(elements));
    }
    // Something with no zero: a string, a slice, a function. Undefined is
    // the honest answer, and reading one is a trap.
    return Value();
  }

  // Where a place points, inside the frame it belongs to. Null when the
  // path does not fit the value -- which the verifier makes impossible and
  // a trap reports rather than a crash.
  // Reading a place: no copies. The elements of an aggregate are shared
  // so that copying a value is cheap, and writing into one needs a copy of
  // its own -- but *reading* must not take that copy, or walking a buffer
  // costs a copy of the buffer per element, which is a hang rather than a
  // slowdown on anything the size of a request.
  static const Value* Read(Frame* frame, const Value& place) {
    const auto* home = static_cast<const Frame*>(
        place.frame() != nullptr ? place.frame() : frame);
    const uint32_t slot = static_cast<uint32_t>(place.integer());
    if (slot >= home->slots.size()) {
      return nullptr;
    }
    const Value* at = &home->slots[slot];
    for (const PlaceStep& step : place.path()) {
      if (at->kind() != Value::Kind::kAggregate ||
          step.index >= at->elements().size()) {
        return nullptr;
      }
      at = &at->elements()[step.index];
    }
    return at;
  }

  static Value* Walk(Frame* frame, const Value& place) {
    // A place names the frame it is in: a slice of a local array is a
    // place, and it is read by the callee it was passed to.
    auto* home = const_cast<Frame*>(static_cast<const Frame*>(
        place.frame() != nullptr ? place.frame() : frame));
    const uint32_t slot = static_cast<uint32_t>(place.integer());
    if (slot >= home->slots.size()) {
      return nullptr;
    }
    Value* at = &home->slots[slot];
    for (const PlaceStep& step : place.path()) {
      if (at->kind() != Value::Kind::kAggregate ||
          step.index >= at->elements().size()) {
        return nullptr;
      }
      // The elements are shared for cheap copying, so writing into one
      // needs a copy of its own first.
      at = at->ElementForWriting(step.index);
      if (at == nullptr) {
        return nullptr;
      }
    }
    return at;
  }

  // One more step along a place. A slot with no path is where it starts.
  static Value Extend(Frame* frame,
                      const Value& base,
                      uint32_t step,
                      bool is_element,
                      TypeId root_type) {
    std::vector<PlaceStep> path;
    if (base.kind() == Value::Kind::kPlace) {
      path = base.path();
      // An element step onto an element step adds: a pointer to element
      // three plus two points at element five. Which is the whole of what
      // pointer arithmetic means, and the compiled tiers get it for free.
      if (is_element && !path.empty() && path.back().is_element) {
        path.back().index += step;
      } else {
        path.push_back(PlaceStep{step, is_element});
      }
      return Value::Place(base.frame(),
                          static_cast<uint32_t>(base.integer()),
                          std::move(path), base.place_type());
    }
    path.push_back(PlaceStep{step, is_element});
    return Value::Place(frame, base.slot(), std::move(path),
                        base.place_type().is_valid() ? base.place_type()
                                                     : root_type);
  }

  // The record an `soa.addr` walks: the element type of the storage its
  // base came from, which the instruction's own type cannot say because
  // that is the field's type.
  TypeId SoaRecordOf(const ir::Function& function,
                     const ir::Inst& inst) const {
    const TypeId base = TypeOf(function, inst, 0);
    if (types_.KindOf(base) == TypeKind::kPointer) {
      return types_.ElementOf(base);
    }
    return base;
  }

  // What an address instruction is taken inside of: a pointer's target, a
  // class's own storage, or -- for a slot -- the slot's type.
  TypeId AddressedType(const ir::Function& function,
                       const ir::Inst& inst,
                       size_t operand) const {
    const TypeId type = TypeOf(function, inst, operand);
    if (types_.IsHandle(type) ||
        types_.KindOf(type) != TypeKind::kPointer) {
      return type;
    }
    return types_.ElementOf(type);
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
          return TrapAt(outcome, TrapKind::kOverflow, function, id,
                "the value does not fit the integer it was cast to");
        }
        return Value::Int(is_signed ? static_cast<uint64_t>(
                                          static_cast<int64_t>(number))
                                    : static_cast<uint64_t>(number),
                          width, is_signed);
      }
      if (source.kind() != Value::Kind::kInt) {
        return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "cast of " + source.Describe());
      }
      // A narrowing cast traps rather than truncating. Overflow is a trap
      // everywhere else in this language, and `as` is a conversion, not a
      // request for the low bits.
      if (!FitsAsInt(source, width, is_signed)) {
        return TrapAt(outcome, TrapKind::kOverflow, function, id,
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
      return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
                "cast of " + source.Describe());
    }

    if (target.kind == TypeKind::kPointer) {
      // One address reinterpreted as another: the only cast the machine
      // does nothing for. The transform that splits a task emits these,
      // because a scheduler holds every frame as the same kind of pointer.
      return source;
    }

    // A cast the IR does not distinguish -- a quantity to its own numeric
    // type, or a type to itself.
    return source;
  }

  // Arithmetic the compiler wrote for itself: the same answer, and no
  // check. Wrapping rather than trapping, which is what an address
  // computation wants and what the compiled tiers do.
  Value Unchecked(Frame* frame, const ir::Inst& inst) {
    const Value& left = Operand(frame, inst, 0);
    const Value& right = Operand(frame, inst, 1);
    const uint64_t a = left.integer();
    const uint64_t b = right.integer();
    uint64_t result = 0;
    switch (inst.opcode) {
      case ir::Opcode::kAdd:
        result = a + b;
        break;
      case ir::Opcode::kSub:
        result = a - b;
        break;
      case ir::Opcode::kMul:
        result = a * b;
        break;
      case ir::Opcode::kBitAnd:
        result = a & b;
        break;
      case ir::Opcode::kShl:
        result = b < 64 ? a << b : 0;
        break;
      default:
        result = a;
        break;
    }
    return Value::Int(result, IntWidth::k64, /*is_signed=*/false);
  }

  // A declaration resolved to the definition it names, which may be in
  // another file: lowering is per file, so a call that crosses one is a
  // declaration here and a body there. By the declaration it came from,
  // not by name -- two files may each define a `Helper`, and this is what
  // the code generator links by too.
  void Link(const ir::Function** callee, const ir::Module** home) const {
    const ir::Function* declaration = *callee;
    if (!declaration->is_declaration() || !declaration->has_source() ||
        declaration->is_foreign()) {
      return;
    }
    for (const ir::Module* other : linked_) {
      if (other == nullptr || other == *home) {
        continue;
      }
      for (const ir::Function& candidate : other->functions) {
        if (candidate.is_declaration() || !candidate.has_source() ||
            candidate.source_file != declaration->source_file ||
            candidate.source_item != declaration->source_item ||
            candidate.source_slot != declaration->source_slot) {
          continue;
        }
        *callee = &candidate;
        *home = other;
        return;
      }
    }
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
      return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
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
          return TrapAt(outcome, TrapKind::kOverflow, function, id,
                std::string(ir::OpcodeMnemonic(inst.opcode)) +
                          " overflowed");
        }
        return Value::Int(raw, width, is_signed);
      }

      case ir::Opcode::kDiv:
      case ir::Opcode::kRem: {
        if (b == 0) {
          return TrapAt(outcome, TrapKind::kDivisionByZero, function, id,
                "divided by zero");
        }
        if (is_signed) {
          const int64_t x = left.signed_integer();
          const int64_t y = right.signed_integer();
          // The one signed division that overflows.
          if (y == -1 && !FitsSigned(-x, width)) {
            return TrapAt(outcome, TrapKind::kOverflow, function, id,
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
          return TrapAt(outcome, TrapKind::kOverflow, function, id,
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
          return TrapAt(outcome, TrapKind::kOverflow, function, id,
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
        return TrapAt(outcome, TrapKind::kUndefinedValue, function, id,
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
          return TrapAt(outcome, TrapKind::kUnreachable, function, id,
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
          return TrapAt(outcome, TrapKind::kUnreachable, function, id,
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
          return TrapAt(outcome, TrapKind::kUnreachable, function, id,
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
        return TrapAt(outcome, TrapKind::kUnreachable, function, id,
                "reached a point the compiler proved unreachable");

      default:
        return TrapAt(outcome, TrapKind::kUnreachable, function, id,
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
  abi::LayoutStore* layouts_ = nullptr;
  std::span<const ir::Module* const> linked_;
  // The systems of the stage being put together, for the tier that runs
  // them itself.
  std::vector<Value> stage_;

  // The class a token names, looked for in the module that is running and
  // then in everything linked with it: the object may have been made
  // somewhere else, and the table that says what its methods are lives
  // with the module that made it.
  const ir::ClassInfo* FindClass(const ir::Module* current,
                                 uint64_t token,
                                 const ir::Module** home) const {
    const auto search = [&](const ir::Module* module) {
      if (module == nullptr) {
        return static_cast<const ir::ClassInfo*>(nullptr);
      }
      for (const ir::ClassInfo& klass : module->classes) {
        if (klass.token == token) {
          *home = module;
          return &klass;
        }
      }
      return static_cast<const ir::ClassInfo*>(nullptr);
    };
    if (const ir::ClassInfo* found = search(current)) {
      return found;
    }
    if (const ir::ClassInfo* found = search(&module_)) {
      return found;
    }
    for (const ir::Module* other : linked_) {
      if (const ir::ClassInfo* found = search(other)) {
        return found;
      }
    }
    return nullptr;
  }
  // Function values handed to the runtime, by the token it holds instead.
  std::vector<Value> tokens_;
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
            ForeignCaller* foreign,
            abi::LayoutStore* layouts,
            std::span<const ir::Module* const> linked) {
  Interpreter interpreter(interner, types, module, limits, output, foreign,
                          layouts, linked);
  return interpreter.Run(function, std::move(arguments));
}

Outcome RunNamed(const StringInterner& interner,
                 const TypeStore& types,
                 const ir::Module& module,
                 Symbol name,
                 std::vector<Value> arguments,
                 const Limits& limits,
                 Output* output,
                 ForeignCaller* foreign,
                 abi::LayoutStore* layouts,
                 std::span<const ir::Module* const> linked) {
  const ir::Function* function = module.Find(name);
  if (function == nullptr) {
    Outcome outcome;
    outcome.trap = TrapKind::kNoBody;
    outcome.where = "@" + std::string(interner.Text(name));
    outcome.message = "is not in the module";
    return outcome;
  }

  // Module-level `var`s get their first values before anything else runs,
  // and start from their type's zero every time something is run: the
  // storage outlives one run, and a differential check that compared a
  // second run against a mutated global would be comparing nothing (D88).
  if (!limits.keep_globals) {
    runtime::wolang_global_reset();
  }
  const auto initialize = [&](const ir::Module& home) {
    if (limits.keep_globals) {
      // The host has them as it wants them.
      return;
    }
    if (home.initializer >= home.functions.size()) {
      return;
    }
    Run(interner, types, home, home.functions[home.initializer], {}, limits,
        output, foreign, layouts, linked);
  };
  for (const ir::Module* other : linked) {
    if (other != nullptr && other != &module) {
      initialize(*other);
    }
  }
  initialize(module);

  return Run(interner, types, module, *function, std::move(arguments),
             limits, output, foreign, layouts, linked);
}

}  // namespace interp
}  // namespace wolang
