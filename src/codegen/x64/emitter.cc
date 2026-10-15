// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/emitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wolang/base/check.h"
#include "wolang/codegen/reg_alloc.h"
#include "wolang/codegen/runtime.h"
#include "wolang/runtime/builtins.h"
#include "wolang/codegen/x64/assembler.h"
#include "wolang/runtime/classes.h"
#include "wolang/runtime/globals.h"

namespace wolang {
namespace codegen {
// Inside the architecture's own namespace rather than
// beside it: two emitters compiled into one translation
// unit -- which is what the amalgamation is -- would
// otherwise share an anonymous namespace and collide on
// every register table in it (D186).
namespace x64 {
namespace {

using x64::Address;
using x64::Assembler;
using x64::Condition;
using x64::Label;
using x64::Register;
using x64::XmmRegister;
using FWidth = x64::FloatWidth;
using IWidth = x64::Width;

// The allocator only hands out callee saved registers, so that a call needs
// no spilling around it. On this machine that is a shorter list than on
// arm64: `rbx` and `r12` through `r15`, of which one holds the context, and
// `rbp` is the frame pointer a backtrace follows.
constexpr Register kIntegerPool[] = {
    Register::kRbx, Register::kR12, Register::kR13, Register::kR14,
};

// There are no callee saved floating point registers in this ABI at all,
// which is a fact about the platform and not a choice: every one of the
// sixteen is the caller's to lose. So nothing floating point is allocated a
// register here -- every float value lives in a frame slot and is loaded
// around each use. It costs two memory operations per operation and it is
// correct, which is the right order to do these in. Spilling only the
// values live across a call is what would fix it, and that is a change to
// the allocator rather than to this file.
constexpr uint32_t kFloatPoolSize = 0;

// Caller saved, so nothing may live in one across a call.
//
// `rax` is first because it is also the accumulator this tier computes
// arithmetic in, and `rcx` and `rdx` are deliberately absent: a variable
// shift needs its amount in `cl` and a division needs `rdx:rax`, so those
// two are reserved rather than fought over. The last four are argument
// registers, which is safe because a scratch never holds anything across
// the setup of a call.
constexpr Register kScratch[] = {
    Register::kRax, Register::kR10, Register::kR11, Register::kRsi,
    Register::kRdi, Register::kR8,  Register::kR9,
};
constexpr XmmRegister kFloatScratch[] = {
    XmmRegister::kXmm8,  XmmRegister::kXmm9,
    XmmRegister::kXmm10, XmmRegister::kXmm11,
    XmmRegister::kXmm12, XmmRegister::kXmm13,
};

constexpr Register kArgumentRegisters[] = {
    Register::kRdi, Register::kRsi, Register::kRdx,
    Register::kRcx, Register::kR8,  Register::kR9,
};
constexpr XmmRegister kFloatArgumentRegisters[] = {
    XmmRegister::kXmm0, XmmRegister::kXmm1, XmmRegister::kXmm2,
    XmmRegister::kXmm3, XmmRegister::kXmm4, XmmRegister::kXmm5,
    XmmRegister::kXmm6, XmmRegister::kXmm7,
};

// The context, which a trap is reported through. Callee saved, so a call
// keeps it without anyone saving it.
constexpr Register kContextRegister = Register::kR15;
// Where arithmetic is done, and where a division has to happen.
constexpr Register kAccumulator = Register::kRax;
// The shift amount and the divisor go here, which is why neither is in the
// scratch list.
constexpr Register kShiftCount = Register::kRcx;
constexpr Register kDivisorHigh = Register::kRdx;
// What a foreign call goes through, and what the trap stub loads the host
// function into.
constexpr Register kCallScratch = Register::kR11;

// A displacement is thirty-two bits on this machine, so the limit is only
// about how much stack one function may take. Four kilobytes is a request
// buffer; the limit has to be well past that.
constexpr uint32_t kMaxFrameBytes = 64 * 1024;

// Where a value of one of these lives.
enum class Holding : uint8_t {
  kNone,
  kInteger,
  kFloat,
  // An address of a frame area: every aggregate, and every `alloca`.
  kArea,
};

struct ValueInfo {
  Holding holding = Holding::kNone;
  uint32_t area = 0;
  Allocation allocation;
};

class FunctionEmitter {
 public:
  FunctionEmitter(const StringInterner& interner,
                  const TypeStore& types,
                  abi::LayoutStore* layouts,
                  const ir::Module& module,
                  const ir::Function& function,
                  const std::vector<bool>& compilable,
                  const std::vector<uint64_t>& foreign,
                  const std::vector<std::pair<uint64_t, uint64_t>>& strings,
                  uint32_t trap_site_base)
      : interner_(interner),
        types_(types),
        layouts_(*layouts),
        module_(module),
        function_(function),
        compilable_(compilable),
        foreign_(foreign),
        strings_(strings),
        trap_site_base_(trap_site_base) {}

  CompiledFunction Run() {
    CompiledFunction result;
    const std::string reason = WhyNot();
    if (!reason.empty()) {
      result.reason = reason;
      return result;
    }

    Prepare();
    const std::string frame_reason = LayOutFrame();
    if (!frame_reason.empty()) {
      result.reason = frame_reason;
      return result;
    }

    EmitEntryStub();
    EmitBody();
    assembler_.Resolve();

    result.compiled = true;
    result.bytes = assembler_.bytes();
    result.stub = 0;
    result.body = body_offset_;
    result.traps = std::move(traps_);
    result.calls = std::move(calls_);
    result.addresses = std::move(addresses_);
    return result;
  }

 private:
  // ------------------------------------------------- can this be compiled --

  std::string WhyNot() {
    if (function_.is_declaration()) {
      return "has no body";
    }
    const TypeId returned = types_.ElementOf(function_.type);
    if (returned.is_valid() && !layouts_.IsSupported(returned)) {
      return "returns " + Describe(returned);
    }
    const std::span<const ir::InstId> params =
        function_.ParamsOf(function_.entry());
    for (ir::InstId param : params) {
      const TypeId type = function_.Get(param).type;
      if (!layouts_.IsSupported(type)) {
        return "takes " + Describe(type);
      }
    }
    // More arguments than registers is not a reason to refuse: the body
    // reads the rest off the stack (D154) and the trampoline puts them
    // there (D194).

    for (const ir::Inst& inst : function_.instructions) {
      if (inst.type.is_valid() && !layouts_.IsSupported(inst.type)) {
        return "works with " + Describe(inst.type);
      }
      switch (inst.opcode) {
        case ir::Opcode::kConstString:
          if (inst.payload.string >= strings_.size()) {
            return "names a string literal that is not in the module";
          }
          break;
        case ir::Opcode::kFuncAddress: {
          // Compiled code can only reach compiled code: there is no way
          // back into the interpreter mid-frame, so a function value has
          // to be one this tier produced.
          const uint32_t callee = inst.payload.callee;
          if (callee < foreign_.size() && foreign_[callee] != 0) {
            break;
          }
          if (callee >= compilable_.size() || !compilable_[callee]) {
            return "takes the address of something this tier cannot "
                   "compile";
          }
          break;
        }
        case ir::Opcode::kCall: {
          const uint32_t callee = inst.payload.callee;
          if (callee < foreign_.size() && foreign_[callee] != 0) {
            break;
          }
          if (callee >= compilable_.size() || !compilable_[callee]) {
            return "calls something this tier cannot compile";
          }
          const std::string reason = CallFits(inst);
          if (!reason.empty()) {
            return reason;
          }
          break;
        }
        case ir::Opcode::kCast: {
          const std::string reason = CastFits(inst);
          if (!reason.empty()) {
            return reason;
          }
          break;
        }
        case ir::Opcode::kEq:
        case ir::Opcode::kNe: {
          const std::span<const ir::InstId> operands =
              function_.OperandsOf(inst);
          if (!operands.empty() &&
              layouts_.ClassOf(function_.Get(operands[0]).type) ==
                  abi::ValueClass::kAggregate) {
            return "compares two aggregates";
          }
          break;
        }
        default:
          break;
      }
    }
    return {};
  }

  std::string CallFits(const ir::Inst& inst) const {
    uint32_t integers = inst.type.is_valid() &&
                                layouts_.ClassOf(inst.type) ==
                                    abi::ValueClass::kAggregate
                            ? 1
                            : 0;
    uint32_t floats = 0;
    for (ir::InstId operand : function_.OperandsOf(inst)) {
      if (layouts_.ClassOf(function_.Get(operand).type) ==
          abi::ValueClass::kFloat) {
        ++floats;
      } else {
        ++integers;
      }
    }
    // Arguments past the registers go in the outgoing area this frame
    // reserved for them, which `EmitCall` fills (D154). Refusing the
    // whole function was left behind from before that worked, and left
    // every program with a call of seven arguments to the interpreter
    // on this machine (D197).
    (void)integers;
    (void)floats;
    return {};
  }

  // The one conversion this machine has no instruction for: everything
  // between a float and a 64-bit unsigned integer. `cvtsi2sd` and
  // `cvttsd2si` are both signed, so a value above the signed range would
  // need a correction sequence; until this tier emits one, the interpreter
  // does it and the answer stays right.
  std::string CastFits(const ir::Inst& inst) const {
    const std::span<const ir::InstId> operands = function_.OperandsOf(inst);
    if (operands.empty()) {
      return {};
    }
    const TypeId from = function_.Get(operands[0]).type;
    const TypeId to = inst.type;
    if (!from.is_valid() || !to.is_valid()) {
      return {};
    }
    const bool from_float = layouts_.ClassOf(from) == abi::ValueClass::kFloat;
    const bool to_float = layouts_.ClassOf(to) == abi::ValueClass::kFloat;
    const auto wide_unsigned = [this](TypeId type) {
      return layouts_.ClassOf(type) == abi::ValueClass::kInteger &&
             layouts_.BitsOf(type) >= 64 && !layouts_.IsSignedInteger(type);
    };
    if ((from_float && wide_unsigned(to)) ||
        (to_float && wide_unsigned(from))) {
      return "converts a 64-bit unsigned integer and a float";
    }
    return {};
  }

  std::string Describe(TypeId type) const {
    return "`" + types_.Describe(interner_, type) + "`";
  }

  // ------------------------------------------------------------- planning --

  void Prepare() {
    const size_t count = function_.instructions.size();
    values_.assign(count, ValueInfo{});
    std::vector<RegClass> classes(count, RegClass::kNone);

    for (size_t i = 0; i < count; ++i) {
      const ir::Inst& inst = function_.instructions[i];
      if (inst.opcode == ir::Opcode::kAlloca) {
        values_[i].holding = Holding::kArea;
        continue;
      }
      if (!ir::ProducesValue(inst.opcode) || !inst.type.is_valid()) {
        continue;
      }
      switch (layouts_.ClassOf(inst.type)) {
        case abi::ValueClass::kInteger:
          values_[i].holding = Holding::kInteger;
          classes[i] = RegClass::kInteger;
          break;
        case abi::ValueClass::kFloat:
          values_[i].holding = Holding::kFloat;
          classes[i] = RegClass::kFloat;
          break;
        case abi::ValueClass::kAggregate:
          values_[i].holding = Holding::kArea;
          break;
        default:
          break;
      }
    }

    order_ = ir::Linearize(function_);
    intervals_ = ComputeIntervals(function_, order_, classes);
    allocation_ = AllocateRegisters(
        intervals_, static_cast<uint32_t>(std::size(kIntegerPool)),
        kFloatPoolSize);
    for (size_t i = 0; i < values_.size(); ++i) {
      if (i < allocation_.values.size()) {
        values_[i].allocation = allocation_.values[i];
      }
    }
  }

  // The bytes a call in this function needs at the stack pointer.
  //
  // Six integer and eight float registers carry arguments on this
  // machine; past those, everything goes on the stack in the order it
  // was written (D154).
  uint32_t OutgoingBytes() const {
    uint32_t most = 0;
    for (const ir::Inst& inst : function_.instructions) {
      if (inst.opcode != ir::Opcode::kCall &&
          inst.opcode != ir::Opcode::kCallIndirect &&
          inst.opcode != ir::Opcode::kVirtualCall) {
        continue;
      }
      const std::span<const ir::InstId> all = function_.OperandsOf(inst);
      const size_t first = inst.opcode == ir::Opcode::kCall ? 0 : 1;
      uint32_t integers = 0;
      uint32_t floats = 0;
      uint32_t spilled = 0;
      // An aggregate result is passed as a hidden first pointer, so it
      // takes an integer register before any argument does.
      if (inst.type.is_valid() &&
          layouts_.ClassOf(inst.type) == abi::ValueClass::kAggregate) {
        ++integers;
      }
      for (size_t i = first; i < all.size(); ++i) {
        const abi::ValueClass value_class =
            layouts_.ClassOf(TypeOf(all[i]));
        if (value_class == abi::ValueClass::kVoid) {
          continue;
        }
        if (value_class == abi::ValueClass::kFloat) {
          if (floats < std::size(kFloatArgumentRegisters)) {
            ++floats;
            continue;
          }
        } else if (integers < std::size(kArgumentRegisters)) {
          ++integers;
          continue;
        }
        ++spilled;
      }
      most = std::max(most, spilled * 8);
    }
    return most;
  }

  std::string LayOutFrame() {
    uint32_t offset = 0;
    // Room for whatever a call in this function passes on the stack, at
    // the bottom of the frame: this ABI wants the first such argument at
    // the stack pointer itself (D154).
    outgoing_bytes_ = abi::AlignUp(OutgoingBytes(), 16);
    offset += outgoing_bytes_;
    saved_integers_ = allocation_.integers_used;
    saved_base_ = offset;
    offset += saved_integers_ * 8;

    spill_base_ = offset;
    offset += allocation_.spill_count * 8;

    uint32_t widest = 0;
    for (const ir::Block& block : function_.blocks) {
      widest = std::max(widest, block.param_count);
    }
    swap_base_ = offset;
    offset += widest * 8;

    // Where the result of an aggregate return goes. It arrives in a
    // register that any call would overwrite, so the frame keeps it.
    const TypeId returned = types_.ElementOf(function_.type);
    indirect_ = returned.is_valid() &&
                layouts_.ClassOf(returned) == abi::ValueClass::kAggregate;
    if (indirect_) {
      indirect_base_ = offset;
      offset += 8;
    }

    // Eight bytes for a virtual call to park the body it looked up while
    // it lays the arguments out. A frame slot rather than a register,
    // because a register held out of the pool costs every loop in the
    // program and virtual calls are rare (D85).
    if (HasVirtualCall()) {
      virtual_target_ = offset;
      offset += 8;
    }

    for (size_t i = 0; i < values_.size(); ++i) {
      if (values_[i].holding != Holding::kArea) {
        continue;
      }
      const TypeId type = function_.instructions[i].type;
      const abi::Layout& layout = layouts_.Of(type);
      if (layout.size == 0) {
        continue;
      }
      offset = abi::AlignUp(offset, std::max(layout.align, 1u));
      values_[i].area = offset;
      if (function_.instructions[i].opcode == ir::Opcode::kAlloca) {
        slots_.push_back(std::pair<uint32_t, uint32_t>(offset, layout.size));
      }
      offset += layout.size;
      if (offset > kMaxFrameBytes) {
        return "needs a frame larger than this tier emits";
      }
    }

    frame_bytes_ = abi::AlignUp(offset, 16);
    if (frame_bytes_ > kMaxFrameBytes) {
      return "needs a frame larger than this tier emits";
    }
    return {};
  }

  // ------------------------------------------------------------ the frame --

  // Whether any instruction here calls through an object.
  bool HasVirtualCall() const {
    for (const ir::Inst& inst : function_.instructions) {
      if (inst.opcode == ir::Opcode::kVirtualCall) {
        return true;
      }
    }
    return false;
  }

  Address Slot(uint32_t offset) const {
    return Address{Register::kRsp, static_cast<int32_t>(offset)};
  }

  uint32_t SpillOffset(const Allocation& allocation) const {
    return spill_base_ + allocation.slot * 8;
  }

  Register PoolRegister(uint32_t index) const {
    WOLANG_CHECK(index < std::size(kIntegerPool));
    return kIntegerPool[index];
  }
  static constexpr uint64_t kNotConstant = ~uint64_t{0};

  // The value of an operand that is a constant, or `kNotConstant`.
  uint64_t ConstantOperand(ir::InstId id) const {
    if (!function_.Contains(id)) {
      return kNotConstant;
    }
    const ir::Inst& inst = function_.Get(id);
    return inst.opcode == ir::Opcode::kConstInt ? inst.payload.integer
                                                : kNotConstant;
  }

  // `x / 2^k` and `x % 2^k`, without a division (D192). The same
  // arithmetic as the other back end, in this one's instructions.
  bool EmitDivideByConstant(ir::InstId id, const ir::Inst& inst,
                            TypeId operand_type, IWidth width,
                            bool is_signed, uint32_t bits,
                            Register left, uint64_t divisor) {
    const bool negative =
        is_signed && (divisor >> (bits - 1)) % 2 == 1;
    const uint64_t mask =
        bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
    const uint64_t magnitude =
        negative ? ((~divisor + 1) & mask) : (divisor & mask);
    if (magnitude == 0 || (magnitude & (magnitude - 1)) != 0) {
      return false;
    }
    unsigned shift = 0;
    while ((magnitude >> shift) != 1) {
      ++shift;
    }

    const Register reg = DestInt(id, 0);
    const Register held = Scratch(0);
    assembler_.Mov(IWidth::k64, held, left);
    if (!is_signed) {
      if (inst.opcode == ir::Opcode::kRem) {
        const Register mask_register = Scratch(1);
        assembler_.MovImmediate(width, mask_register,
                                shift == 0 ? 0 : magnitude - 1);
        if (shift == 0) {
          assembler_.MovImmediate(width, held, 0);
        } else {
          assembler_.And(width, held, mask_register);
        }
      } else if (shift != 0) {
        assembler_.ShrImmediate(width, held,
                                static_cast<uint8_t>(shift));
      }
      if (width == IWidth::k32) {
        Canonicalize(held, operand_type, reg);
      } else {
        assembler_.Mov(IWidth::k64, reg, held);
      }
      FinishInt(id, reg);
      return true;
    }

    // The bias that makes the shift round towards zero: the sign bit
    // smeared across the low `shift` bits.
    if (shift != 0) {
      const Register sign = Scratch(1);
      assembler_.Mov(IWidth::k64, sign, left);
      assembler_.SarImmediate(width, sign,
                              static_cast<uint8_t>(bits - 1));
      assembler_.ShrImmediate(width, sign,
                              static_cast<uint8_t>(bits - shift));
      assembler_.Add(width, held, sign);
      assembler_.SarImmediate(width, held,
                              static_cast<uint8_t>(shift));
    }
    if (negative) {
      if (magnitude == 1) {
        const Register lowest = Scratch(1);
        assembler_.MovImmediate(width, lowest, LowestOf(operand_type));
        assembler_.Cmp(width, left, lowest);
        const Label overflow = TrapLabel(ir::TrapKind::kOverflow, id,
                                         "the quotient does not fit");
        assembler_.Jcc(Condition::kEqual, overflow);
      }
      assembler_.Neg(width, held);
    }
    if (inst.opcode == ir::Opcode::kRem) {
      // `x - (x / d) * d`, with the divisor as it was written.
      const Register scaled = Scratch(1);
      assembler_.MovImmediate(width, scaled, divisor & mask);
      assembler_.Imul(width, held, scaled);
      const Register remainder = Scratch(2);
      assembler_.Mov(IWidth::k64, remainder, left);
      assembler_.Sub(width, remainder, held);
      assembler_.Mov(IWidth::k64, held, remainder);
    }
    if (width == IWidth::k32) {
      Canonicalize(held, operand_type, reg);
    } else {
      assembler_.Mov(IWidth::k64, reg, held);
    }
    FinishInt(id, reg);
    return true;
  }

  Register Scratch(unsigned index) const {
    WOLANG_CHECK(index < std::size(kScratch));
    return kScratch[index];
  }
  XmmRegister FloatScratch(unsigned index) const {
    WOLANG_CHECK(index < std::size(kFloatScratch));
    return kFloatScratch[index];
  }

  // --------------------------------------------------------- value access --

  const ValueInfo& Info(ir::InstId id) const {
    WOLANG_CHECK(id.is_valid() && id.index < values_.size());
    return values_[id.index];
  }

  TypeId TypeOf(ir::InstId id) const {
    return function_.Get(id).type;
  }

  IWidth IntWidthOf(TypeId type) const {
    return layouts_.BitsOf(type) == 64 ? IWidth::k64 : IWidth::k32;
  }
  FWidth FloatWidthOf(TypeId type) const {
    return layouts_.BitsOf(type) == 64 ? FWidth::k64 : FWidth::k32;
  }

  Register UseInt(ir::InstId id, unsigned scratch) {
    const ValueInfo& info = Info(id);
    if (info.allocation.in_register()) {
      return PoolRegister(info.allocation.reg);
    }
    const Register target = Scratch(scratch);
    assembler_.Load(IWidth::k64, target,
                    Slot(SpillOffset(info.allocation)));
    return target;
  }

  XmmRegister UseFloat(ir::InstId id, unsigned scratch) {
    const ValueInfo& info = Info(id);
    const XmmRegister target = FloatScratch(scratch);
    // Nothing floating point is ever in a register here, so this is always
    // a load. Kept in the same shape as the integer case so that the two
    // emitters read alike.
    assembler_.LoadFloat(FWidth::k64, target,
                         Slot(SpillOffset(info.allocation)));
    return target;
  }

  Register UseArea(ir::InstId id, unsigned scratch) {
    const Register target = Scratch(scratch);
    assembler_.Lea(target, Slot(Info(id).area));
    return target;
  }

  // What an address instruction is taken inside of: a pointer's target, a
  // class's own storage, or -- for a slot, whose value is where it lives --
  // the slot's own type.
  TypeId AddressedType(ir::InstId base) const {
    const TypeId type = TypeOf(base);
    if (types_.IsHandle(type) ||
        types_.KindOf(type) != TypeKind::kPointer) {
      return type;
    }
    return types_.ElementOf(type);
  }

  // The record `soa.addr` walks: the element of the storage its base came
  // from, which the instruction's own type cannot say -- that is the
  // field's type.
  TypeId SoaRecordOf(ir::InstId base) const {
    const TypeId type = TypeOf(base);
    return types_.KindOf(type) == TypeKind::kPointer
               ? types_.ElementOf(type)
               : type;
  }

  // How many bytes |count| records take one after another.
  void AosBytes(TypeId storage, Register count, Register target) {
    const Register scratch = Scratch(3);
    assembler_.MovImmediate(IWidth::k64, scratch,
                            layouts_.StrideOf(storage));
    assembler_.Mov(IWidth::k64, kAccumulator, count);
    assembler_.Imul(IWidth::k64, kAccumulator, scratch);
    assembler_.Mov(IWidth::k64, target, kAccumulator);
  }

  // How many bytes |count| records take, one array per field, into
  // |target|. Every field's array holds the same padded number of slots,
  // so the whole thing is the sum of the field sizes times that -- one
  // multiply, not one per field.
  void SoaBytes(TypeId record, Register count, Register target) {
    const Register padded = Scratch(3);
    PaddedCount(count, padded);
    assembler_.MovImmediate(
        IWidth::k64, kAccumulator,
        layouts_.SoaPrefixOf(record, layouts_.FieldCount(record)));
    assembler_.Imul(IWidth::k64, kAccumulator, padded);
    assembler_.Mov(IWidth::k64, target, kAccumulator);
  }

  // `count` rounded up to a multiple of sixteen: what every field's array
  // really holds, so that each one starts 16-byte aligned.
  void PaddedCount(Register count, Register target) {
    assembler_.Mov(IWidth::k64, target, count);
    assembler_.AddImmediate(IWidth::k64, target, 15);
    const Register mask = Scratch(6);
    assembler_.MovImmediate(IWidth::k64, mask, ~uint64_t{15});
    assembler_.And(IWidth::k64, target, mask);
  }

  // The address of field |field| of element |index| in storage holding
  // |count| records.
  void SoaFieldAddress(TypeId record,
                       uint32_t field,
                       Register base,
                       Register index,
                       Register count,
                       Register target) {
    const TypeId type = layouts_.FieldType(record, field);
    const uint32_t size = type.is_valid() ? layouts_.SizeOf(type) : 0;
    const Register padded = Scratch(4);
    PaddedCount(count, padded);
    const Register scratch = Scratch(5);
    assembler_.MovImmediate(IWidth::k64, scratch,
                            layouts_.SoaPrefixOf(record, field));
    assembler_.Mov(IWidth::k64, kAccumulator, padded);
    assembler_.Imul(IWidth::k64, kAccumulator, scratch);
    assembler_.Mov(IWidth::k64, padded, kAccumulator);
    assembler_.MovImmediate(IWidth::k64, scratch, size);
    assembler_.Mov(IWidth::k64, kAccumulator, index);
    assembler_.Imul(IWidth::k64, kAccumulator, scratch);
    assembler_.Add(IWidth::k64, padded, kAccumulator);
    assembler_.Mov(IWidth::k64, target, base);
    assembler_.Add(IWidth::k64, target, padded);
  }

  Register AddressOf(ir::InstId id, unsigned scratch) {
    return Info(id).holding == Holding::kArea ? UseArea(id, scratch)
                                              : UseInt(id, scratch);
  }

  Register DestInt(ir::InstId id, unsigned scratch) {
    const ValueInfo& info = Info(id);
    return info.allocation.in_register() ? PoolRegister(info.allocation.reg)
                                         : Scratch(scratch);
  }
  XmmRegister DestFloat(ir::InstId id, unsigned scratch) {
    return FloatScratch(scratch);
  }

  void FinishInt(ir::InstId id, Register reg) {
    const ValueInfo& info = Info(id);
    if (!info.allocation.in_register()) {
      assembler_.Store(IWidth::k64, Slot(SpillOffset(info.allocation)), reg);
    }
  }
  void FinishFloat(ir::InstId id, XmmRegister reg) {
    assembler_.StoreFloat(FWidth::k64, Slot(SpillOffset(Info(id).allocation)),
                          reg);
  }

  // ------------------------------------------------------------ the traps --

  Label TrapLabel(ir::TrapKind kind, ir::InstId where, std::string message) {
    TrapSite site;
    site.kind = kind;
    site.where = "@" + std::string(interner_.Text(function_.name)) + ":%" +
                 std::to_string(where.index);
    site.message = std::move(message);
    site.span = function_.SpanOf(where);
    const uint32_t index = trap_site_base_ +
                           static_cast<uint32_t>(traps_.size());
    traps_.push_back(std::move(site));

    const Label entry = assembler_.CreateLabel();
    pending_traps_.push_back(PendingTrap{entry, kind, index});
    return entry;
  }

  void TrapNow(ir::TrapKind kind, ir::InstId where, std::string message) {
    const Label label = TrapLabel(kind, where, std::move(message));
    assembler_.Jmp(label);
  }

  void EmitTrapSites() {
    if (pending_traps_.empty()) {
      return;
    }
    const Label stub = assembler_.CreateLabel();
    for (const PendingTrap& trap : pending_traps_) {
      assembler_.Bind(trap.label);
      assembler_.MovImmediate(IWidth::k32, Register::kRsi,
                              static_cast<uint32_t>(trap.kind));
      assembler_.MovImmediate(IWidth::k32, Register::kRdx, trap.site);
      assembler_.Jmp(stub);
    }
    assembler_.Bind(stub);
    assembler_.Mov(IWidth::k64, Register::kRdi, kContextRegister);
    // The frame pointer, so the host can walk the chain of callers.
    assembler_.Mov(IWidth::k64, Register::kRcx, Register::kRbp);
    assembler_.Load(IWidth::k64, kCallScratch,
                    Address{kContextRegister,
                            static_cast<int32_t>(kContextReportOffset)});
    assembler_.CallRegister(kCallScratch);
    // The host never returns from that call: it leaves through the landing
    // pad the entry set up.
    assembler_.Int3();
  }

  // ----------------------------------------------------------- the stub ---

  // The trampoline a host call enters: the context in `rdi`, the arguments
  // in an array in `rsi`, somewhere to put the result in `rdx`.
  void EmitEntryStub() {
    const std::span<const ir::InstId> params =
        function_.ParamsOf(function_.entry());
    body_label_ = assembler_.CreateLabel();

    // Three pushes from an entry that is eight past an alignment boundary
    // leaves the stack aligned for the call below, which this ABI requires
    // and some of what the host calls into enforces.
    assembler_.Push(Register::kRbp);
    assembler_.Mov(IWidth::k64, Register::kRbp, Register::kRsp);
    assembler_.Push(kContextRegister);
    assembler_.Push(Register::kRdx);

    const Register arguments = Register::kR10;
    assembler_.Mov(IWidth::k64, arguments, Register::kRsi);
    assembler_.Mov(IWidth::k64, kContextRegister, Register::kRdi);

    const TypeId returned = types_.ElementOf(function_.type);
    const bool indirect = returned.is_valid() &&
                          layouts_.ClassOf(returned) ==
                              abi::ValueClass::kAggregate;
    uint32_t integers = 0;
    uint32_t floats = 0;
    if (indirect) {
      // Where to write the result is the hidden first argument.
      assembler_.Mov(IWidth::k64, kArgumentRegisters[integers++],
                     Register::kRdx);
    }
    // How many do not fit: the body reads those off the stack (D154),
    // and this is the side that puts them there. A host call to a
    // function of eleven arguments wrote past the end of the register
    // table before this counted.
    uint32_t spilled = 0;
    {
      uint32_t counted_integers = integers;
      uint32_t counted_floats = floats;
      for (const ir::InstId param : params) {
        if (layouts_.ClassOf(TypeOf(param)) == abi::ValueClass::kFloat) {
          if (counted_floats < std::size(kFloatArgumentRegisters)) {
            ++counted_floats;
            continue;
          }
        } else if (counted_integers < std::size(kArgumentRegisters)) {
          ++counted_integers;
          continue;
        }
        ++spilled;
      }
    }
    // Sixteen-byte aligned, because the three pushes above left it that
    // way and the call below needs it kept.
    const uint32_t spill_bytes = abi::AlignUp(spilled * 8, 16);
    if (spill_bytes != 0) {
      assembler_.SubImmediate(IWidth::k64, Register::kRsp, spill_bytes);
    }

    uint32_t placed = 0;
    for (size_t i = 0; i < params.size(); ++i) {
      const TypeId type = TypeOf(params[i]);
      const Address slot{arguments, static_cast<int32_t>(i * 8)};
      const bool is_float =
          layouts_.ClassOf(type) == abi::ValueClass::kFloat;
      const bool on_stack =
          is_float ? floats >= std::size(kFloatArgumentRegisters)
                   : integers >= std::size(kArgumentRegisters);
      if (on_stack) {
        // Through a scratch register: the value is in the array and has
        // to be in a word of the outgoing area.
        assembler_.Load(IWidth::k64, Register::kR11, slot);
        assembler_.Store(
            IWidth::k64,
            Address{Register::kRsp, static_cast<int32_t>(placed * 8)},
            Register::kR11);
        ++placed;
        continue;
      }
      if (is_float) {
        assembler_.LoadFloat(FloatWidthOf(type),
                             kFloatArgumentRegisters[floats++], slot);
      } else {
        assembler_.Load(IWidth::k64, kArgumentRegisters[integers++], slot);
      }
    }

    assembler_.CallRelative(body_label_);
    if (spill_bytes != 0) {
      // Back where it was, so the pushes above are still where they were
      // put.
      assembler_.AddImmediate(IWidth::k64, Register::kRsp, spill_bytes);
    }

    assembler_.Pop(Register::kRdx);
    if (!indirect && returned.is_valid()) {
      if (layouts_.ClassOf(returned) == abi::ValueClass::kFloat) {
        assembler_.StoreFloat(FWidth::k64, Address{Register::kRdx, 0},
                              XmmRegister::kXmm0);
      } else if (layouts_.ClassOf(returned) != abi::ValueClass::kVoid) {
        assembler_.Store(IWidth::k64, Address{Register::kRdx, 0},
                         Register::kRax);
      }
    }
    assembler_.Pop(kContextRegister);
    assembler_.Pop(Register::kRbp);
    assembler_.Ret();
  }

  // ------------------------------------------------------------- the body --

  void EmitBody() {
    assembler_.Bind(body_label_);
    body_offset_ = assembler_.offset();

    labels_.assign(function_.blocks.size(), Label{});
    for (ir::BlockId block : order_.blocks) {
      labels_[block.index] = assembler_.CreateLabel();
    }
    epilogue_ = assembler_.CreateLabel();

    EmitPrologue();

    for (size_t i = 0; i < order_.blocks.size(); ++i) {
      const ir::BlockId block = order_.blocks[i];
      next_block_ =
          i + 1 < order_.blocks.size() ? order_.blocks[i + 1] : ir::BlockId{};
      assembler_.Bind(labels_[block.index]);
      const ir::Block& body = function_.Get(block);
      for (size_t index = body.param_count; index < body.instructions.size();
           ++index) {
        const ir::InstId id = body.instructions[index];
        Emit(id, function_.Get(id));
      }
    }

    assembler_.Bind(epilogue_);
    EmitEpilogue();
    EmitTrapSites();
  }

  void EmitPrologue() {
    assembler_.Push(Register::kRbp);
    assembler_.Mov(IWidth::k64, Register::kRbp, Register::kRsp);
    if (frame_bytes_ > 0) {
      assembler_.SubImmediate(IWidth::k64, Register::kRsp,
                              static_cast<int32_t>(frame_bytes_));
    }
    for (uint32_t i = 0; i < saved_integers_; ++i) {
      assembler_.Store(IWidth::k64, Slot(saved_base_ + i * 8),
                       kIntegerPool[i]);
    }

    // A zero register is one of the things this machine does not have, so
    // zero is made once and then used. Nothing in the prologue depends on
    // the flags, which is the only reason an `xor` is free to be here.
    const bool needs_zero = !slots_.empty();
    if (needs_zero) {
      assembler_.Xor(IWidth::k32, kAccumulator, kAccumulator);
    }
    // A slot starts as zeroes: the interpreter traps on reading one before
    // anything was written, and zeroing at least makes the difference
    // deterministic. `E0123` is what removes it.
    for (const std::pair<uint32_t, uint32_t>& slot : slots_) {
      for (uint32_t written = 0; written < slot.second; written += 8) {
        const uint32_t left = slot.second - written;
        const Address at = Slot(slot.first + written);
        if (left >= 8) {
          assembler_.Store(IWidth::k64, at, kAccumulator);
        } else if (left >= 4) {
          assembler_.Store(IWidth::k32, at, kAccumulator);
        } else if (left >= 2) {
          assembler_.Store(IWidth::k16, at, kAccumulator);
        } else {
          assembler_.Store(IWidth::k8, at, kAccumulator);
        }
      }
    }

    const std::span<const ir::InstId> params =
        function_.ParamsOf(function_.entry());
    uint32_t integers = 0;
    uint32_t floats = 0;
    if (indirect_) {
      assembler_.Store(IWidth::k64, Slot(indirect_base_),
                       kArgumentRegisters[integers++]);
    }
    uint32_t spilled = 0;
    for (ir::InstId param : params) {
      const TypeId type = TypeOf(param);
      const abi::ValueClass value_class = layouts_.ClassOf(type);

      // Past the registers: the caller left it above this frame, with
      // the saved frame pointer and the return address in between
      // (D154).
      const bool on_stack =
          value_class == abi::ValueClass::kFloat
              ? floats >= std::size(kFloatArgumentRegisters)
              : (value_class != abi::ValueClass::kVoid &&
                 integers >= std::size(kArgumentRegisters));
      if (on_stack) {
        const Address where =
            Slot(frame_bytes_ + 16 + spilled * 8);
        ++spilled;
        if (value_class == abi::ValueClass::kFloat) {
          assembler_.LoadFloat(FWidth::k64, XmmRegister::kXmm15, where);
          FinishFloat(param, XmmRegister::kXmm15);
          continue;
        }
        // rax, r10 and r11 only: the rest of the scratch list is
        // argument registers, and the arguments are still in them.
        const Register scratch = Scratch(1);
        assembler_.Load(IWidth::k64, scratch, where);
        if (value_class == abi::ValueClass::kAggregate) {
          const Register area = Scratch(0);
          assembler_.Lea(area, Slot(Info(param).area));
          CopyBytes(area, 0, scratch, 0, layouts_.SizeOf(type), 2);
          continue;
        }
        if (Info(param).allocation.in_register()) {
          assembler_.Mov(IWidth::k64,
                         PoolRegister(Info(param).allocation.reg),
                         scratch);
        } else {
          FinishInt(param, scratch);
        }
        continue;
      }

      if (value_class == abi::ValueClass::kFloat) {
        FinishFloat(param, kFloatArgumentRegisters[floats++]);
        continue;
      }
      const Register source = kArgumentRegisters[integers++];
      if (value_class == abi::ValueClass::kAggregate) {
        // An aggregate arrives as a pointer and is copied into this
        // frame's area, so that every aggregate value is addressed the
        // same way wherever it came from.
        const Register area = Scratch(1);
        assembler_.Lea(area, Slot(Info(param).area));
        CopyBytes(area, 0, source, 0, layouts_.SizeOf(type), 2);
        continue;
      }
      if (Info(param).allocation.in_register()) {
        assembler_.Mov(IWidth::k64,
                       PoolRegister(Info(param).allocation.reg), source);
      } else {
        FinishInt(param, source);
      }
    }
  }

  void EmitEpilogue() {
    for (uint32_t i = 0; i < saved_integers_; ++i) {
      assembler_.Load(IWidth::k64, kIntegerPool[i],
                      Slot(saved_base_ + i * 8));
    }
    // From the frame pointer rather than by adding back, so the epilogue is
    // correct however the body left the stack pointer.
    assembler_.Mov(IWidth::k64, Register::kRsp, Register::kRbp);
    assembler_.Pop(Register::kRbp);
    assembler_.Ret();
  }

  void CopyBytes(Register destination,
                 int32_t destination_offset,
                 Register source,
                 int32_t source_offset,
                 uint32_t size,
                 unsigned scratch) {
    const Register temp = Scratch(scratch);
    uint32_t done = 0;
    while (done < size) {
      const uint32_t left = size - done;
      const IWidth width = left >= 8 ? IWidth::k64
                                     : left >= 4 ? IWidth::k32
                                                 : left >= 2 ? IWidth::k16
                                                             : IWidth::k8;
      const uint32_t step = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
      assembler_.Load(width, temp,
                      Address{source,
                              source_offset + static_cast<int32_t>(done)});
      assembler_.Store(width,
                       Address{destination,
                               destination_offset +
                                   static_cast<int32_t>(done)},
                       temp);
      done += step;
    }
  }

  // ---------------------------------------------------- scalar memory ops --

  void LoadScalar(TypeId type,
                  ir::InstId destination,
                  Register base,
                  int32_t offset) {
    const Address at{base, offset};
    if (layouts_.ClassOf(type) == abi::ValueClass::kFloat) {
      const XmmRegister reg = DestFloat(destination, 0);
      assembler_.LoadFloat(FloatWidthOf(type), reg, at);
      FinishFloat(destination, reg);
      return;
    }
    const Register reg = DestInt(destination, 0);
    const uint32_t bits = layouts_.BitsOf(type);
    const bool is_signed = layouts_.IsSignedInteger(type);
    switch (bits) {
      case 8:
        if (is_signed) {
          assembler_.LoadSigned(IWidth::k8, reg, at);
        } else {
          assembler_.LoadUnsigned(IWidth::k8, reg, at);
        }
        break;
      case 16:
        if (is_signed) {
          assembler_.LoadSigned(IWidth::k16, reg, at);
        } else {
          assembler_.LoadUnsigned(IWidth::k16, reg, at);
        }
        break;
      case 64:
        assembler_.Load(IWidth::k64, reg, at);
        break;
      default:
        // A 32-bit load zeroes the top half, which is the canonical form
        // of an unsigned value; a signed one is sign extended, because
        // that is what a 64-bit compare needs it to be.
        if (is_signed) {
          assembler_.LoadSigned(IWidth::k32, reg, at);
        } else {
          assembler_.Load(IWidth::k32, reg, at);
        }
        break;
    }
    FinishInt(destination, reg);
  }

  void StoreScalar(TypeId type,
                   Register value,
                   Register base,
                   int32_t offset) {
    const Address at{base, offset};
    switch (layouts_.BitsOf(type)) {
      case 8:
        assembler_.Store(IWidth::k8, at, value);
        break;
      case 16:
        assembler_.Store(IWidth::k16, at, value);
        break;
      case 64:
        assembler_.Store(IWidth::k64, at, value);
        break;
      default:
        assembler_.Store(IWidth::k32, at, value);
        break;
    }
  }

  void StoreInto(ir::InstId value,
                 Register base,
                 int32_t offset,
                 unsigned scratch) {
    const TypeId type = TypeOf(value);
    switch (layouts_.ClassOf(type)) {
      case abi::ValueClass::kFloat: {
        const XmmRegister reg = UseFloat(value, scratch);
        assembler_.StoreFloat(FloatWidthOf(type), Address{base, offset}, reg);
        break;
      }
      case abi::ValueClass::kAggregate: {
        const Register source = UseArea(value, scratch);
        CopyBytes(base, offset, source, 0, layouts_.SizeOf(type),
                  scratch + 1);
        break;
      }
      case abi::ValueClass::kVoid:
        break;
      default: {
        const Register reg = UseInt(value, scratch);
        StoreScalar(type, reg, base, offset);
        break;
      }
    }
  }

  // ------------------------------------------------------- instructions ---

  void Emit(ir::InstId id, const ir::Inst& inst) {
    switch (inst.opcode) {
      case ir::Opcode::kParam:
        return;

      case ir::Opcode::kConstInt: {
        const Register reg = DestInt(id, 0);
        assembler_.MovImmediate(IWidth::k64, reg,
                                CanonicalBits(inst.payload.integer,
                                              inst.type));
        FinishInt(id, reg);
        return;
      }
      case ir::Opcode::kConstBool: {
        const Register reg = DestInt(id, 0);
        assembler_.MovImmediate(IWidth::k32, reg,
                                inst.payload.boolean ? 1 : 0);
        FinishInt(id, reg);
        return;
      }
      case ir::Opcode::kConstFloat: {
        const XmmRegister reg = DestFloat(id, 0);
        if (FloatWidthOf(inst.type) == FWidth::k64) {
          uint64_t pattern = 0;
          const double value = inst.payload.floating;
          std::memcpy(&pattern, &value, sizeof(pattern));
          assembler_.MovImmediate(IWidth::k64, kAccumulator, pattern);
          assembler_.MovGeneralToFloat(IWidth::k64, reg, kAccumulator);
        } else {
          uint32_t pattern = 0;
          const float value = static_cast<float>(inst.payload.floating);
          std::memcpy(&pattern, &value, sizeof(pattern));
          assembler_.MovImmediate(IWidth::k32, kAccumulator, pattern);
          assembler_.MovGeneralToFloat(IWidth::k32, reg, kAccumulator);
        }
        FinishFloat(id, reg);
        return;
      }
      case ir::Opcode::kConstString: {
        const std::pair<uint64_t, uint64_t>& literal =
            strings_[inst.payload.string];
        const Register area = UseArea(id, 0);
        const Register value = Scratch(1);
        assembler_.MovImmediate(IWidth::k64, value, literal.first);
        assembler_.Store(IWidth::k64, Address{area, 0}, value);
        assembler_.MovImmediate(IWidth::k64, value, literal.second);
        assembler_.Store(IWidth::k64, Address{area, 8}, value);
        return;
      }

      case ir::Opcode::kConstNull: {
        // Scratch one, not zero: zero is the accumulator, and the tag
        // written below would otherwise overwrite the address it is to
        // be written to (D196).
        const Register area = UseArea(id, 1);
        assembler_.Xor(IWidth::k32, kAccumulator, kAccumulator);
        assembler_.Store(IWidth::k8, Address{area, 0}, kAccumulator);
        return;
      }
      case ir::Opcode::kUndef:
        TrapNow(ir::TrapKind::kUndefinedValue, id,
                "an undefined value was used");
        return;

      case ir::Opcode::kAlloca:
        return;

      case ir::Opcode::kFuncAddress: {
        // The function's own address, left as a placeholder: where the
        // code will live is not known until every function is placed.
        const Register reg = DestInt(id, 0);
        addresses_.push_back(
            AddressFixup{assembler_.offset(), inst.payload.callee});
        // The ten-byte form, whatever the value, so that a patch cannot
        // change how much room it needs.
        assembler_.MovImmediate(IWidth::k64, reg, ~uint64_t{0});
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kNew: {
        // Storage for one object, of the size this tier works out itself.
        const uint64_t allocator = reinterpret_cast<uint64_t>(
            runtime::LookupSymbol("wolang_object_alloc"));
        assembler_.MovImmediate(IWidth::k64, kArgumentRegisters[0],
                                layouts_.ObjectOf(
                                    types_.KindOf(inst.type) ==
                                            TypeKind::kPointer
                                        ? types_.ElementOf(inst.type)
                                        : inst.type)
                                    .size);
        assembler_.MovImmediate(IWidth::k64, kCallScratch, allocator);
        assembler_.CallRegister(kCallScratch);
        const Label trap = TrapLabel(ir::TrapKind::kOutOfMemory, id,
                                     "out of memory");
        assembler_.Test(IWidth::k64, Register::kRax, Register::kRax);
        assembler_.Jcc(Condition::kEqual, trap);
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, reg, Register::kRax);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kLoad: {
        const ir::InstId slot = function_.OperandsOf(inst)[0];
        const Register base = AddressOf(slot, 1);
        if (layouts_.ClassOf(inst.type) == abi::ValueClass::kAggregate) {
          const Register area = UseArea(id, 2);
          CopyBytes(area, 0, base, 0, layouts_.SizeOf(inst.type), 3);
          return;
        }
        LoadScalar(inst.type, id, base, 0);
        return;
      }
      case ir::Opcode::kStore: {
        const std::span<const ir::InstId> operands =
            function_.OperandsOf(inst);
        const Register base = AddressOf(operands[0], 1);
        StoreInto(operands[1], base, 0, 2);
        return;
      }

      case ir::Opcode::kMakeVariant: {
        // The tag, then what the variant carries at the payload offset
        // the whole enum was laid out with (D89).
        const Register area = UseArea(id, 0);
        const Register tag = Scratch(1);
        assembler_.MovImmediate(IWidth::k32, tag, inst.field);
        assembler_.Store(IWidth::k32, Address{area, 0}, tag);
        uint32_t position = 0;
        for (ir::InstId operand : function_.OperandsOf(inst)) {
          const int32_t offset = static_cast<int32_t>(
              layouts_.VariantFieldOffset(inst.type, inst.field, position));
          StoreInto(operand, area, offset, 2);
          ++position;
        }
        return;
      }

      case ir::Opcode::kVariantPayload: {
        const ir::InstId source = function_.OperandsOf(inst)[0];
        const Register base = UseArea(source, 1);
        const int32_t offset = static_cast<int32_t>(
            layouts_.VariantFieldOffset(
                TypeOf(source), inst.field,
                static_cast<uint32_t>(inst.payload.integer)));
        if (layouts_.ClassOf(inst.type) == abi::ValueClass::kAggregate) {
          const Register area = UseArea(id, 2);
          CopyBytes(area, 0, base, offset, layouts_.SizeOf(inst.type), 3);
          return;
        }
        LoadScalar(inst.type, id, base, offset);
        return;
      }

      case ir::Opcode::kMakeStruct:
      case ir::Opcode::kMakeArray: {
        const Register area = UseArea(id, 0);
        const abi::Layout& layout = layouts_.Of(inst.type);
        const bool is_array = types_.KindOf(inst.type) == TypeKind::kArray;
        const uint32_t stride = is_array ? layouts_.StrideOf(inst.type) : 0;
        uint32_t index = 0;
        for (ir::InstId operand : function_.OperandsOf(inst)) {
          const int32_t offset = static_cast<int32_t>(
              is_array ? index * stride
                       : (index < layout.offsets.size()
                              ? layout.offsets[index]
                              : 0));
          StoreInto(operand, area, offset, 1);
          ++index;
        }
        return;
      }
      case ir::Opcode::kExtract: {
        const ir::InstId source = function_.OperandsOf(inst)[0];
        const abi::Layout& layout = layouts_.Of(TypeOf(source));
        const int32_t offset = static_cast<int32_t>(
            inst.field < layout.offsets.size() ? layout.offsets[inst.field]
                                               : 0);
        const Register base = UseArea(source, 1);
        if (layouts_.ClassOf(inst.type) == abi::ValueClass::kAggregate) {
          const Register area = UseArea(id, 2);
          CopyBytes(area, 0, base, offset, layouts_.SizeOf(inst.type), 3);
          return;
        }
        LoadScalar(inst.type, id, base, offset);
        return;
      }
      case ir::Opcode::kInsert: {
        const std::span<const ir::InstId> operands =
            function_.OperandsOf(inst);
        const abi::Layout& layout = layouts_.Of(inst.type);
        const Register area = UseArea(id, 0);
        const Register source = UseArea(operands[0], 1);
        CopyBytes(area, 0, source, 0, layouts_.SizeOf(inst.type), 2);
        const int32_t offset = static_cast<int32_t>(
            inst.field < layout.offsets.size() ? layout.offsets[inst.field]
                                               : 0);
        StoreInto(operands[1], area, offset, 1);
        return;
      }
      case ir::Opcode::kIndex:
        EmitIndex(id, inst);
        return;

      case ir::Opcode::kFieldAddress: {
        const ir::InstId base = function_.OperandsOf(inst)[0];
        const abi::Layout& layout = layouts_.ObjectOf(AddressedType(base));
        const uint32_t offset =
            inst.field < layout.offsets.size() ? layout.offsets[inst.field]
                                               : 0;
        const Register address = AddressOf(base, 1);
        const Register reg = DestInt(id, 0);
        assembler_.Lea(reg, Address{address, static_cast<int32_t>(offset)});
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kTypeSize: {
        // A constant here: this tier knows its own layout at compile
        // time, which is the whole reason the question is an instruction
        // rather than a call (D100).
        const TypeId measured{static_cast<uint32_t>(inst.payload.integer)};
        const Register reg = DestInt(id, 0);
        assembler_.MovImmediate(
            IWidth::k64, reg,
            measured.is_valid() ? std::max(layouts_.SizeOf(measured), 1u)
                                : 1u);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kSoaNew: {
        const std::span<const ir::InstId> operands =
            function_.OperandsOf(inst);
        const TypeId record = types_.ElementOf(inst.type);
        const Register count = UseInt(operands[0], 1);
        if (types_.Get(inst.type).is_soa) {
          SoaBytes(record, count, kArgumentRegisters[0]);
        } else {
          // One record after another: the stride times the count.
          AosBytes(inst.type, count, kArgumentRegisters[0]);
        }
        const uint64_t allocator = reinterpret_cast<uint64_t>(
            runtime::LookupSymbol("wolang_object_alloc"));
        assembler_.MovImmediate(IWidth::k64, kCallScratch, allocator);
        assembler_.CallRegister(kCallScratch);
        const Label trap = TrapLabel(ir::TrapKind::kOutOfMemory, id,
                                     "out of memory");
        assembler_.Test(IWidth::k64, Register::kRax, Register::kRax);
        assembler_.Jcc(Condition::kEqual, trap);
        const Register area = UseArea(id, 1);
        assembler_.Store(IWidth::k64, Address{area, 0}, Register::kRax);
        assembler_.Store(IWidth::k64, Address{area, 8},
                         UseInt(operands[0], 2));
        return;
      }

      case ir::Opcode::kSoaAddress: {
        const std::span<const ir::InstId> operands =
            function_.OperandsOf(inst);
        const TypeId record = SoaRecordOf(operands[0]);
        const Register index = UseInt(operands[1], 1);
        const Register count = UseInt(operands[2], 2);
        const Label trap = TrapLabel(ir::TrapKind::kOutOfBounds, id,
                                     "index is outside the length");
        assembler_.Cmp(IWidth::k64, index, count);
        assembler_.Jcc(Condition::kAboveEqual, trap);

        const Register base = UseInt(operands[0], 3);
        const Register reg = DestInt(id, 0);
        SoaFieldAddress(record, inst.field, base, index, count, reg);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kIndexCheck: {
        // All that is left where an element address was expanded into
        // arithmetic: a compare and a branch, which stays with the
        // element it is about.
        const std::span<const ir::InstId> operands =
            function_.OperandsOf(inst);
        const Register index = UseInt(operands[0], 1);
        const Label trap = TrapLabel(ir::TrapKind::kOutOfBounds, id,
                                     "index is outside the length");
        assembler_.Cmp(IWidth::k64, index, UseInt(operands[1], 2));
        assembler_.Jcc(Condition::kAboveEqual, trap);
        return;
      }

      case ir::Opcode::kIndexAddress: {
        // The element of an array at an address: the index times the
        // stride, bounds checked the same way reading one is.
        const std::span<const ir::InstId> operands =
            function_.OperandsOf(inst);
        // The stride is the size of what the *result* points at, not of
        // what the base is: the base of an element address is an array
        // when the index is into a slot and the element itself when it is
        // into a slice, and only the result is the same either way.
        const uint32_t stride = layouts_.StrideOf(inst.type);
        const Register index = UseInt(operands[1], 1);
        // The length is an operand, because a slice's length is a value
        // rather than something the type knows.
        const Register limit = UseInt(operands[2], 2);
        assembler_.Cmp(IWidth::k64, index, limit);
        const Label trap = TrapLabel(ir::TrapKind::kOutOfBounds, id,
                                     "index is outside the length");
        assembler_.Jcc(Condition::kAboveEqual, trap);

        // The offset goes in scratch one rather than the accumulator,
        // because a spilled destination *is* the accumulator -- it is the
        // first scratch -- and `mov reg, base` would then throw the
        // offset away and `add reg, reg` would answer twice the base
        // (D196).
        const Register offset = Scratch(1);
        assembler_.Mov(IWidth::k64, offset, index);
        assembler_.MovImmediate(IWidth::k64, Scratch(2), stride);
        assembler_.Imul(IWidth::k64, offset, Scratch(2));
        const Register address = AddressOf(operands[0], 3);
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, reg, address);
        assembler_.Add(IWidth::k64, reg, offset);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kOptionalWrap: {
        const ir::InstId inside = function_.OperandsOf(inst)[0];
        const abi::Layout& layout = layouts_.Of(inst.type);
        // Scratch one rather than zero, which is the accumulator the tag
        // goes in (D196).
        const Register area = UseArea(id, 1);
        assembler_.MovImmediate(IWidth::k32, kAccumulator, 1);
        assembler_.Store(IWidth::k8, Address{area, 0}, kAccumulator);
        StoreInto(inside, area,
                  static_cast<int32_t>(layout.offsets.size() > 1
                                           ? layout.offsets[1]
                                           : 0),
                  2);
        return;
      }
      case ir::Opcode::kOptionalIsPresent: {
        const ir::InstId optional = function_.OperandsOf(inst)[0];
        const Register base = UseArea(optional, 1);
        const Register reg = DestInt(id, 0);
        assembler_.LoadUnsigned(IWidth::k8, reg, Address{base, 0});
        FinishInt(id, reg);
        return;
      }
      case ir::Opcode::kOptionalValue: {
        const ir::InstId optional = function_.OperandsOf(inst)[0];
        const abi::Layout& layout = layouts_.Of(TypeOf(optional));
        const Register base = UseArea(optional, 1);
        const Register tag = Scratch(2);
        assembler_.LoadUnsigned(IWidth::k8, tag, Address{base, 0});
        const Label trap =
            TrapLabel(ir::TrapKind::kUndefinedValue, id,
                      "read the value of an optional that is absent");
        assembler_.Test(IWidth::k32, tag, tag);
        assembler_.Jcc(Condition::kEqual, trap);
        const int32_t offset = static_cast<int32_t>(
            layout.offsets.size() > 1 ? layout.offsets[1] : 0);
        if (layouts_.ClassOf(inst.type) == abi::ValueClass::kAggregate) {
          const Register area = UseArea(id, 3);
          CopyBytes(area, 0, base, offset, layouts_.SizeOf(inst.type), 4);
          return;
        }
        LoadScalar(inst.type, id, base, offset);
        return;
      }

      case ir::Opcode::kLocalAddress: {
        // Where the slot is: the frame area the layout reserved for it
        // (D94).
        const ir::InstId slot = function_.OperandsOf(inst)[0];
        const Register reg = DestInt(id, 0);
        assembler_.Lea(reg, Slot(Info(slot).area));
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kGlobalAddress: {
        // Asked for rather than baked in, for the reason above: one
        // storage for both tiers (D88).
        const TypeId held = types_.ElementOf(inst.type);
        assembler_.MovImmediate(IWidth::k64, kArgumentRegisters[0],
                                inst.payload.integer);
        assembler_.MovImmediate(
            IWidth::k64, kArgumentRegisters[1],
            held.is_valid() ? std::max(layouts_.SizeOf(held), 1u) : 8u);
        assembler_.MovImmediate(
            IWidth::k64, kCallScratch,
            reinterpret_cast<uint64_t>(&runtime::wolang_global_address));
        assembler_.CallRegister(kCallScratch);
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, reg, Register::kRax);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kVirtualCall:
        EmitVirtualCall(id, inst);
        return;

      case ir::Opcode::kClassTable: {
        // The number that says which class an object is: its declaration,
        // which the module worked out and this bakes in.
        const Register reg = DestInt(id, 0);
        assembler_.MovImmediate(IWidth::k64, reg, TokenOf(inst));
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kCall:
      case ir::Opcode::kCallIndirect:
        EmitCall(id, inst);
        return;

      case ir::Opcode::kCast:
        EmitCast(id, inst);
        return;

      case ir::Opcode::kLogicalNot: {
        const Register source = UseInt(function_.OperandsOf(inst)[0], 1);
        const Register reg = DestInt(id, 0);
        assembler_.CmpImmediate(IWidth::k32, source, 0);
        SetFlag(Condition::kEqual, reg);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kReturn:
      case ir::Opcode::kJump:
      case ir::Opcode::kBranch:
      case ir::Opcode::kSwitch:
      case ir::Opcode::kUnreachable:
        EmitTerminator(id, inst);
        return;

      default:
        EmitArithmetic(id, inst);
        return;
    }
  }

  // A condition into a whole register. `setcc` writes one byte, so the
  // zero extension is not optional: the rest of the register is whatever
  // was there before.
  void SetFlag(Condition condition, Register destination) {
    assembler_.Setcc(condition, kAccumulator);
    assembler_.Movzx(IWidth::k8, destination, kAccumulator);
  }

  uint64_t CanonicalBits(uint64_t bits, TypeId type) const {
    const uint32_t width = layouts_.BitsOf(type);
    if (width >= 64) {
      return bits;
    }
    const uint64_t mask = (uint64_t{1} << width) - 1;
    uint64_t value = bits & mask;
    if (layouts_.IsSignedInteger(type) &&
        (value & (uint64_t{1} << (width - 1))) != 0) {
      value |= ~mask;
    }
    return value & 0xFFFFFFFFu;
  }

  void EmitIndex(ir::InstId id, const ir::Inst& inst) {
    const std::span<const ir::InstId> operands = function_.OperandsOf(inst);
    const TypeId array = TypeOf(operands[0]);
    const uint64_t length = types_.Get(array).length;
    const uint32_t stride = layouts_.StrideOf(array);

    const Register index = UseInt(operands[1], 1);
    const Register limit = Scratch(2);
    assembler_.MovImmediate(IWidth::k64, limit, length);
    // An unsigned compare catches a negative index too: it reads as a very
    // large one, which is the same answer the interpreter gives.
    assembler_.Cmp(IWidth::k64, index, limit);
    const Label trap = TrapLabel(
        ir::TrapKind::kOutOfBounds, id,
        "index is outside a length of " + std::to_string(length));
    assembler_.Jcc(Condition::kAboveEqual, trap);

    // The index times the stride, in the accumulator so that neither
    // operand's register is disturbed.
    assembler_.Mov(IWidth::k64, kAccumulator, index);
    assembler_.MovImmediate(IWidth::k64, Scratch(2), stride);
    assembler_.Imul(IWidth::k64, kAccumulator, Scratch(2));
    const Register base = UseArea(operands[0], 3);
    assembler_.Add(IWidth::k64, base, kAccumulator);
    if (layouts_.ClassOf(inst.type) == abi::ValueClass::kAggregate) {
      const Register area = UseArea(id, 4);
      CopyBytes(area, 0, base, 0, layouts_.SizeOf(inst.type), 5);
      return;
    }
    LoadScalar(inst.type, id, base, 0);
  }

  // The token the module recorded for a `class.table`.
  uint64_t TokenOf(const ir::Inst& inst) const {
    const uint32_t index = inst.payload.klass;
    return index < module_.classes.size() ? module_.classes[index].token
                                          : 0;
  }

  // A call through the object rather than through the name. The object's
  // first word is its class, the runtime knows where each class's methods
  // ended up, and the body is asked for before any argument register is
  // written (D85).
  void EmitVirtualCall(ir::InstId id, const ir::Inst& inst) {
    const std::span<const ir::InstId> all = function_.OperandsOf(inst);
    if (all.empty()) {
      TrapNow(ir::TrapKind::kUndefinedValue, id,
              "a virtual call with no receiver");
      return;
    }

    // Operand zero is the token -- the number that says which class the
    // object is -- and lowering has already read it out of the object.
    // This read it a second time, as if the token were the handle, and
    // called the lookup with whatever the first field happened to hold
    // (D197).
    const Register token = UseInt(all[0], 0);
    assembler_.Mov(IWidth::k64, kArgumentRegisters[0], token);
    assembler_.MovImmediate(IWidth::k64, kArgumentRegisters[1],
                            inst.payload.integer);
    assembler_.MovImmediate(
        IWidth::k64, kCallScratch,
        reinterpret_cast<uint64_t>(&runtime::wolang_class_lookup));
    assembler_.CallRegister(kCallScratch);
    assembler_.Store(IWidth::k64, Slot(virtual_target_), Register::kRax);
    {
      const Label missing =
          TrapLabel(ir::TrapKind::kNoBody, id,
                    "a virtual call found no compiled body for this "
                    "method");
      assembler_.CmpImmediate(IWidth::k64, Register::kRax, 0);
      assembler_.Jcc(Condition::kEqual, missing);
    }

    const TypeId returned = inst.type;
    const bool indirect = returned.is_valid() &&
                          layouts_.ClassOf(returned) ==
                              abi::ValueClass::kAggregate;
    uint32_t integers = 0;
    uint32_t floats = 0;
    if (indirect) {
      assembler_.Lea(kArgumentRegisters[integers++], Slot(Info(id).area));
    }
    for (size_t index = 1; index < all.size(); ++index) {
      const ir::InstId operand = all[index];
      const TypeId type = TypeOf(operand);
      switch (layouts_.ClassOf(type)) {
        case abi::ValueClass::kFloat:
          assembler_.LoadFloat(FWidth::k64,
                               kFloatArgumentRegisters[floats++],
                               Slot(SpillOffset(Info(operand).allocation)));
          break;
        case abi::ValueClass::kAggregate:
          assembler_.Lea(kArgumentRegisters[integers++],
                         Slot(Info(operand).area));
          break;
        case abi::ValueClass::kVoid:
          break;
        default: {
          const Register source = UseInt(operand, 0);
          assembler_.Mov(IWidth::k64, kArgumentRegisters[integers++],
                         source);
          break;
        }
      }
    }

    assembler_.Load(IWidth::k64, kCallScratch, Slot(virtual_target_));
    assembler_.CallRegister(kCallScratch);
    StoreResult(id, returned, indirect);
  }

  void EmitCall(ir::InstId id, const ir::Inst& inst) {
    const bool indirect_callee = inst.opcode == ir::Opcode::kCallIndirect;
    const std::span<const ir::InstId> all = function_.OperandsOf(inst);
    // Through a value the first operand is the function; the rest are the
    // arguments. Loaded before any argument register is written, because
    // the value may well be in one.
    if (indirect_callee && !all.empty()) {
      assembler_.Mov(IWidth::k64, kCallScratch, UseInt(all[0], 0));
    }
    const TypeId returned = inst.type;
    const bool indirect = returned.is_valid() &&
                          layouts_.ClassOf(returned) ==
                              abi::ValueClass::kAggregate;
    uint32_t integers = 0;
    uint32_t floats = 0;
    if (indirect) {
      assembler_.Lea(kArgumentRegisters[integers++], Slot(Info(id).area));
    }

    uint32_t spilled = 0;
    for (size_t index = indirect_callee ? 1 : 0; index < all.size();
         ++index) {
      const ir::InstId operand = all[index];
      const TypeId type = TypeOf(operand);
      const abi::ValueClass value_class = layouts_.ClassOf(type);
      if (value_class == abi::ValueClass::kVoid) {
        continue;
      }
      const bool on_stack =
          value_class == abi::ValueClass::kFloat
              ? floats >= std::size(kFloatArgumentRegisters)
              : integers >= std::size(kArgumentRegisters);
      if (on_stack) {
        const Address where = Slot(spilled * 8);
        ++spilled;
        if (value_class == abi::ValueClass::kFloat) {
          assembler_.LoadFloat(FWidth::k64, XmmRegister::kXmm15,
                               Slot(SpillOffset(
                                   Info(operand).allocation)));
          assembler_.StoreFloat(FWidth::k64, where, XmmRegister::kXmm15);
          continue;
        }
        if (value_class == abi::ValueClass::kAggregate) {
          const Register area = Scratch(0);
          assembler_.Lea(area, Slot(Info(operand).area));
          assembler_.Store(IWidth::k64, where, area);
          continue;
        }
        assembler_.Store(IWidth::k64, where, UseInt(operand, 0));
        continue;
      }
      switch (value_class) {
        case abi::ValueClass::kFloat: {
          // Loaded straight into the argument register: a float is always
          // in a frame slot on this machine, so there is no move to make.
          assembler_.LoadFloat(FWidth::k64,
                               kFloatArgumentRegisters[floats++],
                               Slot(SpillOffset(Info(operand).allocation)));
          break;
        }
        case abi::ValueClass::kAggregate:
          assembler_.Lea(kArgumentRegisters[integers++],
                         Slot(Info(operand).area));
          break;
        default: {
          // Through the accumulator, which is never an argument register,
          // so that loading one argument cannot overwrite another.
          const Register source = UseInt(operand, 0);
          assembler_.Mov(IWidth::k64, kArgumentRegisters[integers++],
                         source);
          break;
        }
      }
    }

    if (indirect_callee) {
      assembler_.CallRegister(kCallScratch);
      StoreResult(id, returned, indirect);
      return;
    }
    const uint64_t address = inst.payload.callee < foreign_.size()
                                 ? foreign_[inst.payload.callee]
                                 : 0;
    if (address != 0) {
      // Out of the language: the address came from the host's loader, and
      // it is too far away for a relative call. The arguments are already
      // where the platform ABI wants them.
      assembler_.MovImmediate(IWidth::k64, kCallScratch, address);
      assembler_.CallRegister(kCallScratch);
    } else {
      calls_.push_back(
          CallFixup{assembler_.offset(), inst.payload.callee});
      assembler_.CallPlaceholder();
    }

    StoreResult(id, returned, indirect);
  }

  // Where a call's result goes. An aggregate was written through the
  // hidden first argument, so there is nothing to move.
  void StoreResult(ir::InstId id, TypeId returned, bool indirect) {
    if (indirect || !returned.is_valid()) {
      return;
    }
    switch (layouts_.ClassOf(returned)) {
      case abi::ValueClass::kFloat:
        FinishFloat(id, XmmRegister::kXmm0);
        break;
      case abi::ValueClass::kVoid:
        break;
      default: {
        const Register reg = DestInt(id, 0);
        // Only as wide as the type: this ABI lets a function that
        // answers a `bool` or an `s32` leave whatever it likes in the
        // rest of the register, and a `bool` whose high bits were
        // someone else's arithmetic is a `bool` that is true after it
        // was set false (D198).
        if (layouts_.ClassOf(returned) == abi::ValueClass::kInteger &&
            layouts_.BitsOf(returned) < 64) {
          Canonicalize(Register::kRax, returned, reg);
        } else {
          assembler_.Mov(IWidth::k64, reg, Register::kRax);
        }
        FinishInt(id, reg);
        break;
      }
    }
  }

  void EmitTerminator(ir::InstId id, const ir::Inst& inst) {
    switch (inst.opcode) {
      case ir::Opcode::kReturn: {
        if (inst.operands_count != 0) {
          const ir::InstId value = function_.OperandsOf(inst)[0];
          const TypeId type = TypeOf(value);
          switch (layouts_.ClassOf(type)) {
            case abi::ValueClass::kFloat:
              assembler_.LoadFloat(FWidth::k64, XmmRegister::kXmm0,
                                   Slot(SpillOffset(Info(value).allocation)));
              break;
            case abi::ValueClass::kAggregate: {
              const Register source = UseArea(value, 1);
              const Register target = Scratch(2);
              assembler_.Load(IWidth::k64, target, Slot(indirect_base_));
              CopyBytes(target, 0, source, 0, layouts_.SizeOf(type), 3);
              break;
            }
            case abi::ValueClass::kVoid:
              break;
            default:
              assembler_.Mov(IWidth::k64, Register::kRax, UseInt(value, 1));
              break;
          }
        }
        assembler_.Jmp(epilogue_);
        return;
      }

      case ir::Opcode::kJump: {
        const ir::BlockId target = function_.SuccessorsOf(inst)[0];
        EmitEdge(inst, target);
        JumpTo(target);
        return;
      }

      case ir::Opcode::kBranch: {
        const std::span<const ir::BlockId> targets =
            function_.SuccessorsOf(inst);
        const Register condition = UseInt(function_.OperandsOf(inst)[0], 0);
        assembler_.Test(IWidth::k32, condition, condition);
        if (targets[1] == next_block_) {
          assembler_.Jcc(Condition::kNotEqual, labels_[targets[0].index]);
          JumpTo(targets[1]);
          return;
        }
        assembler_.Jcc(Condition::kEqual, labels_[targets[1].index]);
        JumpTo(targets[0]);
        return;
      }

      case ir::Opcode::kSwitch: {
        const std::span<const ir::BlockId> targets =
            function_.SuccessorsOf(inst);
        const std::span<const uint64_t> cases = function_.CasesOf(inst);
        const Register subject = UseInt(function_.OperandsOf(inst)[0], 0);
        const Register value = Scratch(1);
        for (size_t i = 0; i < cases.size() && i + 1 < targets.size(); ++i) {
          assembler_.MovImmediate(IWidth::k64, value, cases[i]);
          assembler_.Cmp(IWidth::k64, subject, value);
          assembler_.Jcc(Condition::kEqual, labels_[targets[i].index]);
        }
        JumpTo(targets.back());
        return;
      }

      case ir::Opcode::kUnreachable:
        TrapNow(ir::TrapKind::kUnreachable, id,
                "reached a point the compiler proved unreachable");
        return;

      default:
        return;
    }
  }

  void JumpTo(ir::BlockId target) {
    if (target == next_block_) {
      return;
    }
    assembler_.Jmp(labels_[target.index]);
  }

  void EmitEdge(const ir::Inst& inst, ir::BlockId target) {
    const std::span<const ir::InstId> operands = function_.OperandsOf(inst);
    const std::span<const ir::InstId> params = function_.ParamsOf(target);
    const size_t count = std::min(operands.size(), params.size());
    if (count == 0) {
      return;
    }

    bool conflict = false;
    for (size_t i = 0; i < count && !conflict; ++i) {
      for (size_t j = 0; j < count; ++j) {
        if (i != j && SameLocation(params[i], operands[j])) {
          conflict = true;
          break;
        }
      }
    }

    if (!conflict) {
      for (size_t i = 0; i < count; ++i) {
        MoveValue(operands[i], params[i]);
      }
      return;
    }

    for (size_t i = 0; i < count; ++i) {
      SpillToSwap(operands[i], static_cast<uint32_t>(i));
    }
    for (size_t i = 0; i < count; ++i) {
      LoadFromSwap(params[i], static_cast<uint32_t>(i));
    }
  }

  bool SameLocation(ir::InstId lhs, ir::InstId rhs) const {
    const ValueInfo& left = Info(lhs);
    const ValueInfo& right = Info(rhs);
    if (left.holding != right.holding) {
      return false;
    }
    if (left.holding == Holding::kArea) {
      return left.area == right.area;
    }
    if (left.allocation.in_register() && right.allocation.in_register()) {
      return left.allocation.reg == right.allocation.reg;
    }
    if (left.allocation.is_spilled() && right.allocation.is_spilled()) {
      return left.allocation.slot == right.allocation.slot;
    }
    return false;
  }

  void MoveValue(ir::InstId from, ir::InstId to) {
    if (SameLocation(from, to)) {
      return;
    }
    const TypeId type = TypeOf(to);
    switch (layouts_.ClassOf(type)) {
      case abi::ValueClass::kFloat: {
        const XmmRegister source = UseFloat(from, 0);
        FinishFloat(to, source);
        break;
      }
      case abi::ValueClass::kAggregate: {
        const Register source = UseArea(from, 0);
        const Register destination = UseArea(to, 1);
        CopyBytes(destination, 0, source, 0, layouts_.SizeOf(type), 2);
        break;
      }
      case abi::ValueClass::kVoid:
        break;
      default: {
        const Register source = UseInt(from, 0);
        const Register destination = DestInt(to, 1);
        assembler_.Mov(IWidth::k64, destination, source);
        FinishInt(to, destination);
        break;
      }
    }
  }

  void SpillToSwap(ir::InstId value, uint32_t index) {
    const uint32_t offset = swap_base_ + index * swap_stride_;
    const TypeId type = TypeOf(value);
    switch (layouts_.ClassOf(type)) {
      case abi::ValueClass::kFloat:
        assembler_.StoreFloat(FWidth::k64, Slot(offset), UseFloat(value, 0));
        break;
      case abi::ValueClass::kAggregate: {
        // Copied into the swap area rather than skipped: the destination
        // is another parameter's area, and leaving it alone left whatever
        // was in it (D107).
        const Register source = UseArea(value, 0);
        const Register room = Scratch(1);
        assembler_.Lea(room, Slot(offset));
        CopyBytes(room, 0, source, 0, layouts_.SizeOf(type), 2);
        break;
      }
      case abi::ValueClass::kVoid:
        break;
      default:
        assembler_.Store(IWidth::k64, Slot(offset), UseInt(value, 0));
        break;
    }
  }

  void LoadFromSwap(ir::InstId value, uint32_t index) {
    const uint32_t offset = swap_base_ + index * swap_stride_;
    const TypeId type = TypeOf(value);
    switch (layouts_.ClassOf(type)) {
      case abi::ValueClass::kFloat: {
        const XmmRegister reg = DestFloat(value, 0);
        assembler_.LoadFloat(FWidth::k64, reg, Slot(offset));
        FinishFloat(value, reg);
        break;
      }
      case abi::ValueClass::kAggregate: {
        // Back out of the swap area into this parameter's own.
        const Register destination = UseArea(value, 0);
        const Register room = Scratch(1);
        assembler_.Lea(room, Slot(static_cast<uint32_t>(offset)));
        CopyBytes(destination, 0, room, 0, layouts_.SizeOf(type), 2);
        break;
      }
      case abi::ValueClass::kVoid:
        break;
      default: {
        const Register reg = DestInt(value, 0);
        assembler_.Load(IWidth::k64, reg, Slot(offset));
        FinishInt(value, reg);
        break;
      }
    }
  }

  // --------------------------------------------------------- conversions --

  void EmitCast(ir::InstId id, const ir::Inst& inst) {
    const ir::InstId source = function_.OperandsOf(inst)[0];
    const TypeId from = TypeOf(source);
    const TypeId to = inst.type;
    const bool from_float = layouts_.ClassOf(from) == abi::ValueClass::kFloat;
    const bool to_float = layouts_.ClassOf(to) == abi::ValueClass::kFloat;

    if (from_float && to_float) {
      const XmmRegister value = UseFloat(source, 1);
      const XmmRegister reg = DestFloat(id, 0);
      if (FloatWidthOf(from) == FloatWidthOf(to)) {
        assembler_.MovFloat(FloatWidthOf(to), reg, value);
      } else {
        assembler_.ConvertFloatWidth(FloatWidthOf(to), reg, value);
      }
      FinishFloat(id, reg);
      return;
    }

    if (!from_float && to_float) {
      // Widened first, so that a narrow unsigned value converts through the
      // signed instruction without its top bit meaning anything.
      const Register value = UseInt(source, 1);
      Extend64(value, from, kAccumulator);
      const XmmRegister reg = DestFloat(id, 0);
      assembler_.IntToFloat(FloatWidthOf(to), reg, IWidth::k64,
                            kAccumulator);
      FinishFloat(id, reg);
      return;
    }

    if (from_float && !to_float) {
      EmitFloatToInteger(id, inst, source, from, to);
      return;
    }

    // One address as another: nothing to do but carry the value across.
    if (types_.KindOf(from) == TypeKind::kPointer &&
        types_.KindOf(to) == TypeKind::kPointer) {
      const Register value = UseInt(source, 1);
      const Register reg = DestInt(id, 0);
      assembler_.Mov(IWidth::k64, reg, value);
      FinishInt(id, reg);
      return;
    }

    // Integer to integer. A conversion that cannot hold the value traps
    // rather than keeping the low bits.
    const Register value = UseInt(source, 1);
    Extend64(value, from, kAccumulator);
    const Register reg = DestInt(id, 0);
    if (layouts_.BitsOf(to) >= 64) {
      assembler_.Mov(IWidth::k64, reg, kAccumulator);
      FinishInt(id, reg);
      return;
    }
    const Register narrowed = Scratch(2);
    Canonicalize(kAccumulator, to, narrowed);
    assembler_.Cmp(IWidth::k64, narrowed, kAccumulator);
    const Label trap =
        TrapLabel(ir::TrapKind::kOverflow, id,
                  "the value does not fit the integer it was cast to");
    assembler_.Jcc(Condition::kNotEqual, trap);
    assembler_.Mov(IWidth::k64, reg, narrowed);
    FinishInt(id, reg);
  }

  // Sign or zero extends a canonical value of |type| to a full 64 bits.
  void Extend64(Register value, TypeId type, Register destination) {
    if (layouts_.BitsOf(type) >= 64) {
      assembler_.Mov(IWidth::k64, destination, value);
      return;
    }
    if (layouts_.IsSignedInteger(type)) {
      assembler_.Movsx(IWidth::k32, destination, value);
    } else {
      // Writing the 32-bit half zeroes the other one, which is the whole
      // of what a zero extension is here.
      assembler_.Mov(IWidth::k32, destination, value);
    }
  }

  void Canonicalize(Register value, TypeId type, Register destination) {
    const uint32_t bits = layouts_.BitsOf(type);
    const bool is_signed = layouts_.IsSignedInteger(type);
    switch (bits) {
      case 8:
        if (is_signed) {
          assembler_.Movsx(IWidth::k8, destination, value);
        } else {
          assembler_.Movzx(IWidth::k8, destination, value);
        }
        return;
      case 16:
        if (is_signed) {
          assembler_.Movsx(IWidth::k16, destination, value);
        } else {
          assembler_.Movzx(IWidth::k16, destination, value);
        }
        return;
      case 32:
        if (is_signed) {
          assembler_.Movsx(IWidth::k32, destination, value);
        } else {
          assembler_.Mov(IWidth::k32, destination, value);
        }
        return;
      default:
        assembler_.Mov(IWidth::k64, destination, value);
        return;
    }
  }

  void EmitFloatToInteger(ir::InstId id,
                          const ir::Inst& inst,
                          ir::InstId source,
                          TypeId from,
                          TypeId to) {
    const XmmRegister value = UseFloat(source, 1);
    const FWidth width = FloatWidthOf(from);
    const Label trap =
        TrapLabel(ir::TrapKind::kOverflow, id,
                  "the value does not fit the integer it was cast to");

    // Not a number converts to nothing, so it is the first thing ruled
    // out. An unordered compare sets the parity flag, which is what makes
    // this one instruction and a branch.
    assembler_.CompareFloat(width, value, value);
    assembler_.Jcc(Condition::kParity, trap);

    const Register raw = Scratch(2);
    assembler_.FloatToInt(IWidth::k64, raw, width, value);

    const uint32_t bits = layouts_.BitsOf(to);
    if (bits >= 64) {
      // The conversion gives the one indefinite value rather than
      // faulting, so a 64-bit target has its range checked against the
      // float itself. Both bounds are exact powers of two, which every
      // float width holds without rounding, so the comparison is exact at
      // the boundary -- and the boundary is where this is easy to get
      // wrong. Only the signed case gets here: the unsigned one is
      // declined, because this machine has no unsigned conversion.
      const XmmRegister limit = FloatScratch(2);
      MaterializeFloat(width, limit, 9223372036854775808.0);
      assembler_.CompareFloat(width, value, limit);
      assembler_.Jcc(Condition::kAboveEqual, trap);
      MaterializeFloat(width, limit, -9223372036854775808.0);
      assembler_.CompareFloat(width, value, limit);
      // The most negative value is in range; anything below it is not.
      assembler_.Jcc(Condition::kBelow, trap);
      const Register whole = DestInt(id, 0);
      assembler_.Mov(IWidth::k64, whole, raw);
      FinishInt(id, whole);
      return;
    }

    const Register narrowed = Scratch(3);
    Canonicalize(raw, to, narrowed);
    assembler_.Cmp(IWidth::k64, narrowed, raw);
    assembler_.Jcc(Condition::kNotEqual, trap);
    const Register reg = DestInt(id, 0);
    assembler_.Mov(IWidth::k64, reg, narrowed);
    FinishInt(id, reg);
    (void)inst;
  }

  void MaterializeFloat(FWidth width,
                        XmmRegister destination,
                        double value) {
    if (width == FWidth::k64) {
      uint64_t pattern = 0;
      std::memcpy(&pattern, &value, sizeof(pattern));
      assembler_.MovImmediate(IWidth::k64, kAccumulator, pattern);
      assembler_.MovGeneralToFloat(IWidth::k64, destination, kAccumulator);
      return;
    }
    uint32_t pattern = 0;
    const float narrow = static_cast<float>(value);
    std::memcpy(&pattern, &narrow, sizeof(pattern));
    assembler_.MovImmediate(IWidth::k32, kAccumulator, pattern);
    assembler_.MovGeneralToFloat(IWidth::k32, destination, kAccumulator);
  }

  // --------------------------------------------------------- arithmetic ---

  void EmitArithmetic(ir::InstId id, const ir::Inst& inst) {
    const std::span<const ir::InstId> operands = function_.OperandsOf(inst);
    if (operands.empty()) {
      TrapNow(ir::TrapKind::kUndefinedValue, id, "has no execution");
      return;
    }
    const TypeId operand_type = TypeOf(operands[0]);
    if (layouts_.ClassOf(operand_type) == abi::ValueClass::kFloat) {
      EmitFloatArithmetic(id, inst, operand_type);
      return;
    }
    EmitIntegerArithmetic(id, inst, operand_type);
  }

  void EmitFloatArithmetic(ir::InstId id,
                           const ir::Inst& inst,
                           TypeId operand_type) {
    const std::span<const ir::InstId> operands = function_.OperandsOf(inst);
    const FWidth width = FloatWidthOf(operand_type);
    const XmmRegister left = UseFloat(operands[0], 1);
    const XmmRegister right =
        operands.size() > 1 ? UseFloat(operands[1], 2) : left;

    switch (inst.opcode) {
      case ir::Opcode::kAdd:
      case ir::Opcode::kSub:
      case ir::Opcode::kMul:
      case ir::Opcode::kDiv:
      case ir::Opcode::kNeg: {
        const XmmRegister reg = DestFloat(id, 0);
        // Two operand arithmetic: the destination is also the left side,
        // so the left side is copied in first.
        if (inst.opcode == ir::Opcode::kNeg) {
          // Flip the sign bit, which is a bitwise operation rather than a
          // subtraction from zero: that would turn a negative zero into a
          // positive one.
          const XmmRegister mask = FloatScratch(3);
          MaterializeFloat(width, mask, -0.0);
          assembler_.MovFloat(width, reg, left);
          assembler_.XorFloat(reg, mask);
          FinishFloat(id, reg);
          return;
        }
        assembler_.MovFloat(width, reg, left);
        switch (inst.opcode) {
          case ir::Opcode::kAdd:
            assembler_.AddFloat(width, reg, right);
            break;
          case ir::Opcode::kSub:
            assembler_.SubFloat(width, reg, right);
            break;
          case ir::Opcode::kMul:
            assembler_.MulFloat(width, reg, right);
            break;
          default:
            // Division by zero is an infinity in IEEE-754, which is
            // defined and not a fault.
            assembler_.DivFloat(width, reg, right);
            break;
        }
        FinishFloat(id, reg);
        return;
      }
      default:
        break;
    }

    // A comparison. Unordered sets the carry, the zero and the parity
    // flags, so the unsigned conditions are the ordered ones: `above` is
    // false for a comparison against a value that is not a number, which
    // is what every ordered test has to be.
    const Register reg = DestInt(id, 0);
    switch (inst.opcode) {
      case ir::Opcode::kGt:
        assembler_.CompareFloat(width, left, right);
        SetFlag(Condition::kAbove, reg);
        break;
      case ir::Opcode::kGe:
        assembler_.CompareFloat(width, left, right);
        SetFlag(Condition::kAboveEqual, reg);
        break;
      case ir::Opcode::kLt:
        // Swapped rather than inverted, for the same reason.
        assembler_.CompareFloat(width, right, left);
        SetFlag(Condition::kAbove, reg);
        break;
      case ir::Opcode::kLe:
        assembler_.CompareFloat(width, right, left);
        SetFlag(Condition::kAboveEqual, reg);
        break;
      case ir::Opcode::kEq:
      case ir::Opcode::kNe: {
        // Equality needs two flags: the values are equal *and* they are
        // ordered. Inequality is the other way around -- not equal *or*
        // unordered -- which is why a comparison with a value that is not
        // a number is unequal to everything including itself.
        assembler_.CompareFloat(width, left, right);
        const bool equal = inst.opcode == ir::Opcode::kEq;
        assembler_.Setcc(equal ? Condition::kEqual : Condition::kNotEqual,
                         kAccumulator);
        assembler_.Setcc(equal ? Condition::kNoParity : Condition::kParity,
                         kShiftCount);
        if (equal) {
          assembler_.And(IWidth::k8, kAccumulator, kShiftCount);
        } else {
          assembler_.Or(IWidth::k8, kAccumulator, kShiftCount);
        }
        assembler_.Movzx(IWidth::k8, reg, kAccumulator);
        break;
      }
      default:
        TrapNow(ir::TrapKind::kUndefinedValue, id, "has no execution");
        return;
    }
    FinishInt(id, reg);
  }

  void EmitIntegerArithmetic(ir::InstId id,
                             const ir::Inst& inst,
                             TypeId operand_type) {
    const std::span<const ir::InstId> operands = function_.OperandsOf(inst);
    const uint32_t bits = layouts_.BitsOf(operand_type);
    const bool is_signed = layouts_.IsSignedInteger(operand_type);
    const IWidth width = bits == 64 ? IWidth::k64 : IWidth::k32;
    const Register left = UseInt(operands[0], 1);
    const Register right =
        operands.size() > 1 ? UseInt(operands[1], 2) : left;

    // Arithmetic the compiler wrote for itself is not checked: a trap in
    // an address computation would name a line the program never wrote.
    if (inst.is_unchecked) {
      const Register reg = DestInt(id, 0);
      assembler_.Mov(IWidth::k64, kAccumulator, left);
      switch (inst.opcode) {
        case ir::Opcode::kAdd:
          assembler_.Add(width, kAccumulator, right);
          break;
        case ir::Opcode::kSub:
          assembler_.Sub(width, kAccumulator, right);
          break;
        case ir::Opcode::kMul:
          assembler_.Imul(IWidth::k64, kAccumulator, right);
          break;
        case ir::Opcode::kBitAnd:
          assembler_.And(width, kAccumulator, right);
          break;
        case ir::Opcode::kShl:
          assembler_.Mov(IWidth::k64, kShiftCount, right);
          assembler_.ShlCl(width, kAccumulator);
          break;
        default:
          break;
      }
      assembler_.Mov(IWidth::k64, reg, kAccumulator);
      FinishInt(id, reg);
      return;
    }

    switch (inst.opcode) {
      case ir::Opcode::kAdd:
      case ir::Opcode::kSub: {
        const bool is_add = inst.opcode == ir::Opcode::kAdd;
        // Computed in the accumulator rather than in the destination: the
        // instruction overwrites its left operand, and the destination's
        // register may well be the right operand's.
        assembler_.Mov(IWidth::k64, kAccumulator, left);
        if (bits >= 32) {
          if (is_add) {
            assembler_.Add(width, kAccumulator, right);
          } else {
            assembler_.Sub(width, kAccumulator, right);
          }
          const Label trap = TrapLabel(
              ir::TrapKind::kOverflow, id,
              std::string(ir::OpcodeMnemonic(inst.opcode)) + " overflowed");
          // Signed overflow and unsigned wraparound are two different
          // flags, and asking for the wrong one is the classic way to get
          // this right for one half of the integers.
          assembler_.Jcc(is_signed ? Condition::kOverflow : Condition::kBelow,
                         trap);
          const Register reg = DestInt(id, 0);
          if (width == IWidth::k32) {
            // A 32-bit result is only canonical once it has been put back
            // in the form its signedness calls for.
            Canonicalize(kAccumulator, operand_type, reg);
          } else {
            assembler_.Mov(IWidth::k64, reg, kAccumulator);
          }
          FinishInt(id, reg);
          return;
        }
        // A narrower width has no flag of its own: do the arithmetic wide
        // and ask whether the answer still fits.
        if (is_add) {
          assembler_.Add(IWidth::k32, kAccumulator, right);
        } else {
          assembler_.Sub(IWidth::k32, kAccumulator, right);
        }
        const Register reg = DestInt(id, 0);
        CheckFits(id, kAccumulator, operand_type, reg, inst.opcode);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kMul: {
        const Register reg = DestInt(id, 0);
        if (bits >= 64) {
          const Label trap =
              TrapLabel(ir::TrapKind::kOverflow, id, "mul overflowed");
          if (is_signed) {
            // The two operand multiply sets the overflow flag itself.
            assembler_.Mov(IWidth::k64, kAccumulator, left);
            assembler_.Imul(IWidth::k64, kAccumulator, right);
            assembler_.Jcc(Condition::kOverflow, trap);
          } else {
            // The widening one puts the high half where it can be tested,
            // which is how an unsigned product says it did not fit.
            assembler_.Mov(IWidth::k64, kAccumulator, left);
            assembler_.Mov(IWidth::k64, kShiftCount, right);
            assembler_.MulWide(IWidth::k64, kShiftCount);
            assembler_.Test(IWidth::k64, kDivisorHigh, kDivisorHigh);
            assembler_.Jcc(Condition::kNotEqual, trap);
          }
          assembler_.Mov(IWidth::k64, reg, kAccumulator);
          FinishInt(id, reg);
          return;
        }
        // Up to 32 bits the product of two extended operands cannot wrap a
        // 64-bit register, so one wide multiply answers both questions.
        const Register wide_right = Scratch(3);
        Extend64(right, operand_type, wide_right);
        Extend64(left, operand_type, kAccumulator);
        assembler_.Imul(IWidth::k64, kAccumulator, wide_right);
        CheckFits(id, kAccumulator, operand_type, reg, inst.opcode);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kDiv:
      case ir::Opcode::kRem: {
        // A constant power of two is a shift, and the zero check goes
        // away with it (D192).
        if (const uint64_t divisor =
                ConstantOperand(function_.OperandsOf(inst)[1]);
            divisor != kNotConstant && divisor != 0) {
          if (EmitDivideByConstant(id, inst, operand_type, width,
                                   is_signed, bits, left, divisor)) {
            return;
          }
        }
        const Label by_zero =
            TrapLabel(ir::TrapKind::kDivisionByZero, id, "divided by zero");
        assembler_.Test(width, right, right);
        assembler_.Jcc(Condition::kEqual, by_zero);
        // The divisor goes somewhere the division itself will not
        // overwrite: this instruction takes `rdx:rax` and gives back both.
        assembler_.Mov(IWidth::k64, kShiftCount, right);
        if (is_signed) {
          // The one signed division that overflows: the most negative
          // value divided by minus one.
          const Label skip = assembler_.CreateLabel();
          assembler_.CmpImmediate(width, kShiftCount, -1);
          assembler_.Jcc(Condition::kNotEqual, skip);
          const Register lowest = Scratch(3);
          assembler_.MovImmediate(width, lowest, LowestOf(operand_type));
          assembler_.Cmp(width, left, lowest);
          const Label overflow = TrapLabel(ir::TrapKind::kOverflow, id,
                                           "the quotient does not fit");
          assembler_.Jcc(Condition::kEqual, overflow);
          assembler_.Bind(skip);
        }
        assembler_.Mov(IWidth::k64, kAccumulator, left);
        if (is_signed) {
          assembler_.Cqo(width);
          assembler_.Idiv(width, kShiftCount);
        } else {
          assembler_.Xor(IWidth::k32, kDivisorHigh, kDivisorHigh);
          assembler_.Div(width, kShiftCount);
        }
        const Register reg = DestInt(id, 0);
        const Register result =
            inst.opcode == ir::Opcode::kRem ? kDivisorHigh : kAccumulator;
        if (width == IWidth::k32) {
          Canonicalize(result, operand_type, reg);
        } else {
          assembler_.Mov(IWidth::k64, reg, result);
        }
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kNeg: {
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, kAccumulator, left);
        if (bits >= 32) {
          assembler_.Neg(width, kAccumulator);
          const Label trap = TrapLabel(ir::TrapKind::kOverflow, id,
                                       "negation overflowed");
          assembler_.Jcc(Condition::kOverflow, trap);
          if (width == IWidth::k32) {
            Canonicalize(kAccumulator, operand_type, reg);
          } else {
            assembler_.Mov(IWidth::k64, reg, kAccumulator);
          }
          FinishInt(id, reg);
          return;
        }
        assembler_.Neg(IWidth::k32, kAccumulator);
        CheckFits(id, kAccumulator, operand_type, reg, inst.opcode);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kBitAnd:
      case ir::Opcode::kBitOr:
      case ir::Opcode::kBitXor: {
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, kAccumulator, left);
        if (inst.opcode == ir::Opcode::kBitAnd) {
          assembler_.And(width, kAccumulator, right);
        } else if (inst.opcode == ir::Opcode::kBitOr) {
          assembler_.Or(width, kAccumulator, right);
        } else {
          assembler_.Xor(width, kAccumulator, right);
        }
        // A bitwise operation on signed operands keeps them canonical, but
        // the 32-bit form zeroed the top half, so put it back.
        if (width == IWidth::k32) {
          Canonicalize(kAccumulator, operand_type, reg);
        } else {
          assembler_.Mov(IWidth::k64, reg, kAccumulator);
        }
        FinishInt(id, reg);
        return;
      }
      case ir::Opcode::kBitNot: {
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, kAccumulator, left);
        assembler_.Not(width, kAccumulator);
        Canonicalize(kAccumulator, operand_type, reg);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kShl:
      case ir::Opcode::kShr: {
        // Compared at the operand's own width: a 64-bit shift amount whose
        // low half looks small is still too large, and the interpreter
        // reads the whole value.
        assembler_.CmpImmediate(width, right, static_cast<int32_t>(bits));
        const Label trap = TrapLabel(ir::TrapKind::kOverflow, id,
                                     "shifted by more than the width");
        assembler_.Jcc(Condition::kAboveEqual, trap);
        // A variable shift takes its amount in one register and nowhere
        // else, which is the reason that register is reserved.
        assembler_.Mov(IWidth::k64, kShiftCount, right);
        assembler_.Mov(IWidth::k64, kAccumulator, left);
        if (inst.opcode == ir::Opcode::kShl) {
          assembler_.ShlCl(width, kAccumulator);
        } else if (is_signed) {
          assembler_.SarCl(width, kAccumulator);
        } else {
          assembler_.ShrCl(width, kAccumulator);
        }
        // A shift truncates rather than faulting, the same way the
        // interpreter masks to the width.
        const Register reg = DestInt(id, 0);
        Canonicalize(kAccumulator, operand_type, reg);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kEq:
      case ir::Opcode::kNe:
      case ir::Opcode::kLt:
      case ir::Opcode::kLe:
      case ir::Opcode::kGt:
      case ir::Opcode::kGe: {
        Condition condition = Condition::kEqual;
        switch (inst.opcode) {
          case ir::Opcode::kEq:
            condition = Condition::kEqual;
            break;
          case ir::Opcode::kNe:
            condition = Condition::kNotEqual;
            break;
          case ir::Opcode::kLt:
            condition = is_signed ? Condition::kLess : Condition::kBelow;
            break;
          case ir::Opcode::kLe:
            condition =
                is_signed ? Condition::kLessEqual : Condition::kBelowEqual;
            break;
          case ir::Opcode::kGt:
            condition = is_signed ? Condition::kGreater : Condition::kAbove;
            break;
          default:
            condition =
                is_signed ? Condition::kGreaterEqual : Condition::kAboveEqual;
            break;
        }
        const Register reg = DestInt(id, 0);
        assembler_.Cmp(width, left, right);
        SetFlag(condition, reg);
        FinishInt(id, reg);
        return;
      }

      default:
        TrapNow(ir::TrapKind::kUndefinedValue, id,
                std::string(ir::OpcodeMnemonic(inst.opcode)) +
                    " has no execution");
        return;
    }
  }

  // Traps unless |raw| still means the same thing at |type|'s width, and
  // leaves the canonical form in |destination|.
  void CheckFits(ir::InstId id,
                 Register raw,
                 TypeId type,
                 Register destination,
                 ir::Opcode opcode) {
    const Register narrowed = Scratch(2);
    Canonicalize(raw, type, narrowed);
    assembler_.Cmp(IWidth::k64, narrowed, raw);
    const Label trap = TrapLabel(
        ir::TrapKind::kOverflow, id,
        std::string(ir::OpcodeMnemonic(opcode)) + " overflowed");
    assembler_.Jcc(Condition::kNotEqual, trap);
    assembler_.Mov(IWidth::k64, destination, narrowed);
  }

  uint64_t LowestOf(TypeId type) const {
    const uint32_t bits = layouts_.BitsOf(type);
    if (bits >= 64) {
      return uint64_t{1} << 63;
    }
    const uint64_t value = uint64_t{1} << (bits - 1);
    return (~(value - 1)) & 0xFFFFFFFFu;
  }

  struct PendingTrap {
    Label label;
    ir::TrapKind kind = ir::TrapKind::kNone;
    uint32_t site = 0;
  };

  const StringInterner& interner_;
  const TypeStore& types_;
  abi::LayoutStore& layouts_;
  // The module this function is in, for what its functions share: the
  // string pool and the class tables.
  const ir::Module& module_;
  const ir::Function& function_;
  const std::vector<bool>& compilable_;
  const std::vector<uint64_t>& foreign_;
  const std::vector<std::pair<uint64_t, uint64_t>>& strings_;
  uint32_t trap_site_base_ = 0;
  // Where a virtual call parks the body it looked up.
  uint32_t virtual_target_ = 0;

  Assembler assembler_;
  std::vector<ValueInfo> values_;
  ir::LinearOrder order_;
  std::vector<Interval> intervals_;
  AllocationResult allocation_;

  std::vector<Label> labels_;
  Label epilogue_;
  Label body_label_;
  ir::BlockId next_block_;
  uint32_t body_offset_ = 0;

  uint32_t saved_base_ = 0;
  uint32_t saved_integers_ = 0;
  uint32_t spill_base_ = 0;
  uint32_t swap_base_ = 0;
  // How wide one swap slot is: eight bytes, or the widest aggregate a
  // block parameter of this function is.
  uint32_t swap_stride_ = 8;
  bool indirect_ = false;
  uint32_t indirect_base_ = 0;
  uint32_t frame_bytes_ = 0;
  uint32_t outgoing_bytes_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> slots_;

  std::vector<TrapSite> traps_;
  std::vector<PendingTrap> pending_traps_;
  std::vector<CallFixup> calls_;
  std::vector<AddressFixup> addresses_;
};

}  // namespace

CompiledFunction EmitFunction(const StringInterner& interner,
                              const TypeStore& types,
                              abi::LayoutStore* layouts,
                              const ir::Module& module,
                              const ir::Function& function,
                              const std::vector<bool>& compilable,
                              const std::vector<uint64_t>& foreign,
                              const std::vector<std::pair<uint64_t,
                                                          uint64_t>>& strings,
                              uint32_t trap_site_base) {
  FunctionEmitter emitter(interner, types, layouts, module, function,
                          compilable, foreign, strings, trap_site_base);
  return emitter.Run();
}

}  // namespace x64

}  // namespace codegen
}  // namespace wolang
