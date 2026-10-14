// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/emitter.h"

#include <algorithm>
#include <cstring>

#include "wolang/base/check.h"
#include "wolang/codegen/arm64/assembler.h"
#include "wolang/codegen/reg_alloc.h"
#include "wolang/codegen/runtime.h"
#include "wolang/runtime/builtins.h"

namespace wolang {
namespace codegen {
namespace {

using arm64::Assembler;
using arm64::Condition;
using arm64::FloatRegister;
using arm64::Label;
using arm64::Register;
using FWidth = arm64::FloatWidth;
using IWidth = arm64::Width;

// The allocator only hands out callee saved registers.
//
// That is the whole reason a call needs no spilling around it: every value
// the allocator placed survives a call because the callee is obliged to
// restore it. The price is nine integer registers and eight floating point
// ones instead of twenty-odd, and a prologue that saves the ones used.
constexpr Register kIntegerPool[] = {
    Register::kX19, Register::kX20, Register::kX21, Register::kX22,
    Register::kX23, Register::kX24, Register::kX25, Register::kX26,
    Register::kX27,
};
constexpr FloatRegister kFloatPool[] = {
    FloatRegister::kV8,  FloatRegister::kV9,  FloatRegister::kV10,
    FloatRegister::kV11, FloatRegister::kV12, FloatRegister::kV13,
    FloatRegister::kV14, FloatRegister::kV15,
};

// Caller saved, so nothing may live in one across a call.
constexpr Register kScratch[] = {
    Register::kX9,  Register::kX10, Register::kX11,
    Register::kX12, Register::kX13, Register::kX14,
};
constexpr FloatRegister kFloatScratch[] = {
    FloatRegister::kV16, FloatRegister::kV17, FloatRegister::kV18,
    FloatRegister::kV19,
};

constexpr Register kArgumentRegisters[] = {
    Register::kX0, Register::kX1, Register::kX2, Register::kX3,
    Register::kX4, Register::kX5, Register::kX6, Register::kX7,
};
constexpr FloatRegister kFloatArgumentRegisters[] = {
    FloatRegister::kV0, FloatRegister::kV1, FloatRegister::kV2,
    FloatRegister::kV3, FloatRegister::kV4, FloatRegister::kV5,
    FloatRegister::kV6, FloatRegister::kV7,
};

// The context, which a trap is reported through. Callee saved, so a call
// keeps it without anyone saving it.
constexpr Register kContextRegister = Register::kX28;
// Where the caller of a function returning an aggregate puts the address to
// write the result to -- the same register the platform ABI uses for an
// indirect return, so the convention is at least not a surprising one.
constexpr Register kIndirectResult = Register::kX8;
// What a call goes through, and what the trap stub loads the host function
// into. Both are the architecture's scratch registers for exactly this.
constexpr Register kCallScratch = Register::kX16;

constexpr uint32_t kMaxFrameBytes = 4000;

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
    const std::vector<uint32_t>& words = assembler_.words();
    result.bytes.resize(words.size() * sizeof(uint32_t));
    if (!words.empty()) {
      std::memcpy(result.bytes.data(), words.data(), result.bytes.size());
    }
    result.stub = 0;
    result.body = body_offset_;
    result.traps = std::move(traps_);
    result.calls = std::move(calls_);
    result.addresses = std::move(addresses_);
    return result;
  }

 private:
  // ------------------------------------------------- can this be compiled --

  // The dev tier compiles what it can. Everything it cannot is named here,
  // in one place, so that "why is this slow" has an answer rather than a
  // guess.
  std::string WhyNot() {
    if (function_.is_declaration()) {
      return "has no body";
    }
    const std::span<const ir::InstId> params =
        function_.ParamsOf(function_.entry());
    uint32_t integers = 0;
    uint32_t floats = 0;
    for (ir::InstId param : params) {
      const TypeId type = function_.Get(param).type;
      if (!layouts_.IsSupported(type)) {
        return "takes " + Describe(type);
      }
      if (layouts_.ClassOf(type) == abi::ValueClass::kFloat) {
        ++floats;
      } else {
        ++integers;
      }
    }
    if (integers > std::size(kArgumentRegisters) ||
        floats > std::size(kFloatArgumentRegisters)) {
      return "takes more arguments than registers";
    }

    const TypeId returned = types_.ElementOf(function_.type);
    if (returned.is_valid() && !layouts_.IsSupported(returned)) {
      return "returns " + Describe(returned);
    }

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
            // A foreign call needs no body of its own, only an address.
            break;
          }
          if (callee >= compilable_.size() || !compilable_[callee]) {
            return "calls something this tier cannot compile";
          }
          break;
        }
        case ir::Opcode::kEq:
        case ir::Opcode::kNe: {
          // The interpreter compares two aggregates structurally. Doing
          // that in machine code needs a field-by-field walk, and until
          // this tier has one, saying so is better than being wrong.
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
        static_cast<uint32_t>(std::size(kFloatPool)));
    for (size_t i = 0; i < values_.size(); ++i) {
      if (i < allocation_.values.size()) {
        values_[i].allocation = allocation_.values[i];
      }
    }
  }

  std::string LayOutFrame() {
    uint32_t offset = 0;
    saved_integers_ = allocation_.integers_used;
    saved_floats_ = allocation_.floats_used;
    saved_base_ = offset;
    offset += (saved_integers_ + saved_floats_) * 8;

    spill_base_ = offset;
    offset += allocation_.spill_count * 8;

    // One swap slot per block parameter, for the one case a jump cannot be
    // done with direct moves: when the values going around a loop have to
    // trade places.
    uint32_t widest = 0;
    for (const ir::Block& block : function_.blocks) {
      widest = std::max(widest, block.param_count);
    }
    swap_base_ = offset;
    offset += widest * 8;

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

  uint32_t SpillOffset(const Allocation& allocation) const {
    return spill_base_ + allocation.slot * 8;
  }

  Register PoolRegister(uint32_t index) const {
    WOLANG_CHECK(index < std::size(kIntegerPool));
    return kIntegerPool[index];
  }
  FloatRegister FloatPoolRegister(uint32_t index) const {
    WOLANG_CHECK(index < std::size(kFloatPool));
    return kFloatPool[index];
  }
  Register Scratch(unsigned index) const {
    WOLANG_CHECK(index < std::size(kScratch));
    return kScratch[index];
  }
  FloatRegister FloatScratch(unsigned index) const {
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

  // The register holding |id|, loading it from its spill slot into
  // |scratch| when that is where it lives.
  Register UseInt(ir::InstId id, unsigned scratch) {
    const ValueInfo& info = Info(id);
    if (info.allocation.in_register()) {
      return PoolRegister(info.allocation.reg);
    }
    const Register target = Scratch(scratch);
    assembler_.Ldr(IWidth::k64, target, Register::kSp,
                   static_cast<int32_t>(SpillOffset(info.allocation)));
    return target;
  }

  FloatRegister UseFloat(ir::InstId id, unsigned scratch) {
    const ValueInfo& info = Info(id);
    if (info.allocation.in_register()) {
      return FloatPoolRegister(info.allocation.reg);
    }
    const FloatRegister target = FloatScratch(scratch);
    assembler_.LdrFloat(FWidth::k64, target, Register::kSp,
                        static_cast<int32_t>(SpillOffset(info.allocation)));
    return target;
  }

  // The address of |id|'s frame area, in |scratch|.
  Register UseArea(ir::InstId id, unsigned scratch) {
    const Register target = Scratch(scratch);
    assembler_.AddImm(IWidth::k64, target, Register::kSp, Info(id).area);
    return target;
  }

  // What an address instruction is taken inside of: a pointer's target, a
  // class's own storage, or -- for a slot, whose value is where it lives --
  // the slot's own type. Taking the wrong one puts every field at offset
  // zero, which reads as a value that is wrong rather than as a program
  // that crashes.
  TypeId AddressedType(ir::InstId base) const {
    const TypeId type = TypeOf(base);
    if (types_.IsHandle(type) ||
        types_.KindOf(type) != TypeKind::kPointer) {
      return type;
    }
    return types_.ElementOf(type);
  }

  // The address a load or a store works through. A slot's address is a
  // frame offset; a pointer's is whatever the pointer holds. Both are
  // addresses, which is why one opcode covers reading a local and reading
  // through a `*T`.
  Register AddressOf(ir::InstId id, unsigned scratch) {
    return Info(id).holding == Holding::kArea ? UseArea(id, scratch)
                                              : UseInt(id, scratch);
  }

  // Where to put the result of an instruction: its own register when it has
  // one, a scratch when it is spilled.
  Register DestInt(ir::InstId id, unsigned scratch) {
    const ValueInfo& info = Info(id);
    return info.allocation.in_register() ? PoolRegister(info.allocation.reg)
                                         : Scratch(scratch);
  }
  FloatRegister DestFloat(ir::InstId id, unsigned scratch) {
    const ValueInfo& info = Info(id);
    return info.allocation.in_register()
               ? FloatPoolRegister(info.allocation.reg)
               : FloatScratch(scratch);
  }

  void FinishInt(ir::InstId id, Register reg) {
    const ValueInfo& info = Info(id);
    if (!info.allocation.in_register()) {
      assembler_.Str(IWidth::k64, reg, Register::kSp,
                     static_cast<int32_t>(SpillOffset(info.allocation)));
    }
  }
  void FinishFloat(ir::InstId id, FloatRegister reg) {
    const ValueInfo& info = Info(id);
    if (!info.allocation.in_register()) {
      assembler_.StrFloat(FWidth::k64, reg, Register::kSp,
                          static_cast<int32_t>(
                              SpillOffset(info.allocation)));
    }
  }

  // ------------------------------------------------------------ the traps --

  // Emits the branch to the trap stub, which is what "the operation faults"
  // compiles to. The message is the interpreter's, word for word: the two
  // tiers are compared on the whole outcome, not only on the kind.
  Label TrapLabel(ir::TrapKind kind, ir::InstId where, std::string message) {
    TrapSite site;
    site.kind = kind;
    site.where = "@" + std::string(interner_.Text(function_.name)) + ":%" +
                 std::to_string(where.index);
    site.message = std::move(message);
    const uint32_t index = trap_site_base_ +
                           static_cast<uint32_t>(traps_.size());
    traps_.push_back(std::move(site));

    const Label entry = assembler_.CreateLabel();
    pending_traps_.push_back(PendingTrap{entry, kind, index});
    return entry;
  }

  void TrapNow(ir::TrapKind kind, ir::InstId where, std::string message) {
    const Label label = TrapLabel(kind, where, std::move(message));
    assembler_.B(label);
  }

  // One trap site's three words, placed after everything else so that the
  // path that does not fault is a not-taken branch and nothing more.
  void EmitTrapSites() {
    if (pending_traps_.empty()) {
      return;
    }
    const Label stub = assembler_.CreateLabel();
    for (const PendingTrap& trap : pending_traps_) {
      assembler_.Bind(trap.label);
      assembler_.MovImmediate(IWidth::k32, Register::kX1,
                              static_cast<uint32_t>(trap.kind));
      assembler_.MovImmediate(IWidth::k32, Register::kX2, trap.site);
      assembler_.B(stub);
    }
    assembler_.Bind(stub);
    assembler_.Mov(IWidth::k64, Register::kX0, kContextRegister);
    // The frame pointer, so the host can walk the chain of callers. It is
    // the only part of the machine state a trap needs that the host cannot
    // work out for itself.
    assembler_.Mov(IWidth::k64, Register::kX3, Register::kX29);
    assembler_.Ldr(IWidth::k64, kCallScratch, kContextRegister,
                   static_cast<int32_t>(kContextReportOffset));
    assembler_.Blr(kCallScratch);
    // The host never returns from that call: it leaves through the landing
    // pad the entry set up. A `brk` says so to a disassembler.
    assembler_.Brk(1);
  }

  // ----------------------------------------------------------- the stub ---

  // The trampoline a host call enters. Loads the arguments out of an array,
  // puts the context in its register, and writes the result back.
  void EmitEntryStub() {
    const std::span<const ir::InstId> params =
        function_.ParamsOf(function_.entry());
    body_label_ = assembler_.CreateLabel();

    assembler_.StpPreIndex(Register::kX29, Register::kX30, Register::kSp,
                           -32);
    assembler_.AddImm(IWidth::k64, Register::kX29, Register::kSp, 0);
    assembler_.Str(IWidth::k64, kContextRegister, Register::kSp, 16);
    // The result address outlives the call, and every register that could
    // hold it is either an argument or caller saved.
    assembler_.Str(IWidth::k64, Register::kX2, Register::kSp, 24);
    assembler_.Mov(IWidth::k64, Register::kX9, Register::kX1);
    assembler_.Mov(IWidth::k64, kContextRegister, Register::kX0);

    const TypeId returned = types_.ElementOf(function_.type);
    const bool indirect = returned.is_valid() &&
                          layouts_.ClassOf(returned) ==
                              abi::ValueClass::kAggregate;
    if (indirect) {
      assembler_.Mov(IWidth::k64, kIndirectResult, Register::kX2);
    }

    uint32_t integers = 0;
    uint32_t floats = 0;
    for (size_t i = 0; i < params.size(); ++i) {
      const TypeId type = TypeOf(params[i]);
      const int32_t slot = static_cast<int32_t>(i * 8);
      if (layouts_.ClassOf(type) == abi::ValueClass::kFloat) {
        assembler_.LdrFloat(FloatWidthOf(type),
                            kFloatArgumentRegisters[floats++], Register::kX9,
                            slot);
      } else {
        assembler_.Ldr(IWidth::k64, kArgumentRegisters[integers++],
                       Register::kX9, slot);
      }
    }

    assembler_.Bl(body_label_);

    if (!indirect && returned.is_valid()) {
      assembler_.Ldr(IWidth::k64, Register::kX9, Register::kSp, 24);
      if (layouts_.ClassOf(returned) == abi::ValueClass::kFloat) {
        assembler_.StrFloat(FWidth::k64, FloatRegister::kV0, Register::kX9,
                            0);
      } else if (layouts_.ClassOf(returned) != abi::ValueClass::kVoid) {
        assembler_.Str(IWidth::k64, Register::kX0, Register::kX9, 0);
      }
    }
    assembler_.Ldr(IWidth::k64, kContextRegister, Register::kSp, 16);
    assembler_.LdpPostIndex(Register::kX29, Register::kX30, Register::kSp,
                            32);
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
    // The frame record, which is not optional even in a function with no
    // frame of its own: a call overwrites the link register, so anything
    // that calls has to have saved it. It is also what a stack walker
    // follows, so the frame pointer is chained whether or not this function
    // needs one.
    assembler_.StpPreIndex(Register::kX29, Register::kX30, Register::kSp,
                           -16);
    assembler_.AddImm(IWidth::k64, Register::kX29, Register::kSp, 0);
    if (frame_bytes_ > 0) {
      assembler_.SubImm(IWidth::k64, Register::kSp, Register::kSp,
                        frame_bytes_);
    }
    for (uint32_t i = 0; i < saved_integers_; ++i) {
      assembler_.Str(IWidth::k64, kIntegerPool[i], Register::kSp,
                     static_cast<int32_t>(saved_base_ + i * 8));
    }
    for (uint32_t i = 0; i < saved_floats_; ++i) {
      assembler_.StrFloat(FWidth::k64, kFloatPool[i], Register::kSp,
                          static_cast<int32_t>(
                              saved_base_ + (saved_integers_ + i) * 8));
    }

    // A slot starts as zeroes. The interpreter traps on reading one before
    // anything was written, which needs a shadow bit per slot to reproduce;
    // zeroing at least makes the difference deterministic, and `E0123` --
    // the use of an uninitialized variable -- is what removes it.
    for (const std::pair<uint32_t, uint32_t>& slot : slots_) {
      for (uint32_t written = 0; written < slot.second; written += 8) {
        if (slot.second - written >= 8) {
          assembler_.Str(IWidth::k64, Register::kZr, Register::kSp,
                         static_cast<int32_t>(slot.first + written));
        } else if (slot.second - written >= 4) {
          assembler_.Str(IWidth::k32, Register::kZr, Register::kSp,
                         static_cast<int32_t>(slot.first + written));
        } else if (slot.second - written >= 2) {
          assembler_.Strh(Register::kZr, Register::kSp,
                          static_cast<int32_t>(slot.first + written));
        } else {
          assembler_.Strb(Register::kZr, Register::kSp,
                          static_cast<int32_t>(slot.first + written));
        }
      }
    }

    // The incoming arguments go where the allocator put them.
    const std::span<const ir::InstId> params =
        function_.ParamsOf(function_.entry());
    uint32_t integers = 0;
    uint32_t floats = 0;
    for (ir::InstId param : params) {
      const TypeId type = TypeOf(param);
      const abi::ValueClass value_class = layouts_.ClassOf(type);
      if (value_class == abi::ValueClass::kFloat) {
        const FloatRegister source = kFloatArgumentRegisters[floats++];
        if (Info(param).allocation.in_register()) {
          assembler_.FMov(FWidth::k64,
                          FloatPoolRegister(Info(param).allocation.reg),
                          source);
        } else {
          FinishFloat(param, source);
        }
        continue;
      }
      const Register source = kArgumentRegisters[integers++];
      if (value_class == abi::ValueClass::kAggregate) {
        // An aggregate arrives as a pointer and is copied into this
        // frame's area, so that every aggregate value is addressed the
        // same way wherever it came from.
        const Register area = Scratch(1);
        assembler_.AddImm(IWidth::k64, area, Register::kSp,
                          Info(param).area);
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
      assembler_.Ldr(IWidth::k64, kIntegerPool[i], Register::kSp,
                     static_cast<int32_t>(saved_base_ + i * 8));
    }
    for (uint32_t i = 0; i < saved_floats_; ++i) {
      assembler_.LdrFloat(FWidth::k64, kFloatPool[i], Register::kSp,
                          static_cast<int32_t>(
                              saved_base_ + (saved_integers_ + i) * 8));
    }
    // `mov sp, x29` rather than an add, so the epilogue is correct however
    // the body left the stack pointer.
    assembler_.AddImm(IWidth::k64, Register::kSp, Register::kX29, 0);
    assembler_.LdpPostIndex(Register::kX29, Register::kX30, Register::kSp,
                            16);
    assembler_.Ret();
  }

  // Copies |size| bytes between two addresses. Eight at a time, because an
  // aggregate is almost always small and a loop would cost more than the
  // copy.
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
      if (left >= 8) {
        assembler_.Ldr(IWidth::k64, temp, source, source_offset + done);
        assembler_.Str(IWidth::k64, temp, destination,
                       destination_offset + done);
        done += 8;
      } else if (left >= 4) {
        assembler_.Ldr(IWidth::k32, temp, source, source_offset + done);
        assembler_.Str(IWidth::k32, temp, destination,
                       destination_offset + done);
        done += 4;
      } else if (left >= 2) {
        assembler_.Ldrh(temp, source, source_offset + done);
        assembler_.Strh(temp, destination, destination_offset + done);
        done += 2;
      } else {
        assembler_.Ldrb(temp, source, source_offset + done);
        assembler_.Strb(temp, destination, destination_offset + done);
        done += 1;
      }
    }
  }

  // ---------------------------------------------------- scalar memory ops --

  void LoadScalar(TypeId type,
                  ir::InstId destination,
                  Register base,
                  int32_t offset) {
    if (layouts_.ClassOf(type) == abi::ValueClass::kFloat) {
      const FloatRegister reg = DestFloat(destination, 0);
      assembler_.LdrFloat(FloatWidthOf(type), reg, base, offset);
      FinishFloat(destination, reg);
      return;
    }
    const Register reg = DestInt(destination, 0);
    const uint32_t bits = layouts_.BitsOf(type);
    const bool is_signed = layouts_.IsSignedInteger(type);
    switch (bits) {
      case 8:
        if (is_signed) {
          assembler_.Ldrsb(IWidth::k32, reg, base, offset);
        } else {
          assembler_.Ldrb(reg, base, offset);
        }
        break;
      case 16:
        if (is_signed) {
          assembler_.Ldrsh(IWidth::k32, reg, base, offset);
        } else {
          assembler_.Ldrh(reg, base, offset);
        }
        break;
      case 64:
        assembler_.Ldr(IWidth::k64, reg, base, offset);
        break;
      default:
        assembler_.Ldr(IWidth::k32, reg, base, offset);
        break;
    }
    FinishInt(destination, reg);
  }

  void StoreScalar(TypeId type,
                   Register value,
                   Register base,
                   int32_t offset) {
    switch (layouts_.BitsOf(type)) {
      case 8:
        assembler_.Strb(value, base, offset);
        break;
      case 16:
        assembler_.Strh(value, base, offset);
        break;
      case 64:
        assembler_.Str(IWidth::k64, value, base, offset);
        break;
      default:
        assembler_.Str(IWidth::k32, value, base, offset);
        break;
    }
  }

  // Writes |value| into an aggregate's field, whatever kind of value it is.
  void StoreInto(ir::InstId value,
                 Register base,
                 int32_t offset,
                 unsigned scratch) {
    const TypeId type = TypeOf(value);
    switch (layouts_.ClassOf(type)) {
      case abi::ValueClass::kFloat: {
        const FloatRegister reg = UseFloat(value, scratch);
        assembler_.StrFloat(FloatWidthOf(type), reg, base, offset);
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
        assembler_.MovImmediate(IntWidthOf(inst.type), reg,
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
        const FloatRegister reg = DestFloat(id, 0);
        const Register bits = Scratch(0);
        if (FloatWidthOf(inst.type) == FWidth::k64) {
          uint64_t pattern = 0;
          const double value = inst.payload.floating;
          std::memcpy(&pattern, &value, sizeof(pattern));
          assembler_.MovImmediate(IWidth::k64, bits, pattern);
          assembler_.FMovFromGeneral(FWidth::k64, reg, bits);
        } else {
          uint32_t pattern = 0;
          const float value = static_cast<float>(inst.payload.floating);
          std::memcpy(&pattern, &value, sizeof(pattern));
          assembler_.MovImmediate(IWidth::k32, bits, pattern);
          assembler_.FMovFromGeneral(FWidth::k32, reg, bits);
        }
        FinishFloat(id, reg);
        return;
      }
      case ir::Opcode::kConstString: {
        // The bytes stay where they are and the value is a view of them,
        // which is what a `str` is.
        const std::pair<uint64_t, uint64_t>& literal =
            strings_[inst.payload.string];
        const Register area = UseArea(id, 0);
        const Register value = Scratch(1);
        assembler_.MovImmediate(IWidth::k64, value, literal.first);
        assembler_.Str(IWidth::k64, value, area, 0);
        assembler_.MovImmediate(IWidth::k64, value, literal.second);
        assembler_.Str(IWidth::k64, value, area, 8);
        return;
      }

      case ir::Opcode::kConstNull: {
        // An absent optional is a zero tag; the payload is never read.
        const Register area = UseArea(id, 0);
        assembler_.Strb(Register::kZr, area, 0);
        return;
      }
      case ir::Opcode::kUndef:
        TrapNow(ir::TrapKind::kUndefinedValue, id,
                "an undefined value was used");
        return;

      case ir::Opcode::kAlloca:
        // The area is the frame slot the layout already reserved.
        return;

      case ir::Opcode::kFuncAddress: {
        // The function's own address, left as a placeholder: where the
        // code will live is not known until every function is placed.
        const Register reg = DestInt(id, 0);
        addresses_.push_back(AddressFixup{
            static_cast<uint32_t>(assembler_.words().size() *
                                  sizeof(uint32_t)),
            inst.payload.callee});
        assembler_.MovAddress(reg, 0);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kNew: {
        // Storage for one object. The size is this tier's own answer --
        // which is why `new` is an operation and not a call lowering
        // wrote, since lowering has no layout.
        const uint64_t allocator = reinterpret_cast<uint64_t>(
            runtime::LookupSymbol("wolang_object_alloc"));
        assembler_.MovImmediate(IWidth::k64, kArgumentRegisters[0],
                                layouts_.ObjectOf(inst.type).size);
        assembler_.MovImmediate(IWidth::k64, kCallScratch, allocator);
        assembler_.Blr(kCallScratch);
        const Label trap = TrapLabel(ir::TrapKind::kOutOfMemory, id,
                                     "out of memory");
        assembler_.Cbz(IWidth::k64, Register::kX0, trap);
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, reg, Register::kX0);
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

      case ir::Opcode::kMakeStruct:
      case ir::Opcode::kMakeArray: {
        const Register area = UseArea(id, 0);
        const abi::Layout& layout = layouts_.Of(inst.type);
        const bool is_array =
            types_.KindOf(inst.type) == TypeKind::kArray;
        const uint32_t stride =
            is_array ? layouts_.StrideOf(inst.type) : 0;
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
      case ir::Opcode::kIndex: {
        EmitIndex(id, inst);
        return;
      }

      case ir::Opcode::kFieldAddress: {
        // An address plus what the layout says, which is the same `add` a
        // field access already emits -- the difference is that the result
        // is the address rather than what is at it.
        const ir::InstId base = function_.OperandsOf(inst)[0];
        const abi::Layout& layout = layouts_.ObjectOf(AddressedType(base));
        const uint32_t offset =
            inst.field < layout.offsets.size() ? layout.offsets[inst.field]
                                               : 0;
        const Register address = AddressOf(base, 1);
        const Register reg = DestInt(id, 0);
        assembler_.AddImm(IWidth::k64, reg, address, offset);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kIndexAddress: {
        // The element of an array at an address: the index times the
        // stride, bounds checked the same way reading one is.
        const std::span<const ir::InstId> operands =
            function_.OperandsOf(inst);
        const TypeId array = AddressedType(operands[0]);
        const uint64_t length = types_.Get(array).length;
        const Register index = UseInt(operands[1], 1);
        const Register limit = Scratch(2);
        assembler_.MovImmediate(IWidth::k64, limit, length);
        assembler_.Cmp(IWidth::k64, index, limit);
        const Label trap = TrapLabel(
            ir::TrapKind::kOutOfBounds, id,
            "index is outside a length of " + std::to_string(length));
        assembler_.BCond(Condition::kHs, trap);

        const Register address = AddressOf(operands[0], 3);
        const Register offset = Scratch(2);
        assembler_.MovImmediate(IWidth::k64, offset,
                                layouts_.StrideOf(array));
        assembler_.Mul(IWidth::k64, offset, index, offset);
        const Register reg = DestInt(id, 0);
        assembler_.Add(IWidth::k64, reg, address, offset);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kOptionalWrap: {
        const ir::InstId inside = function_.OperandsOf(inst)[0];
        const abi::Layout& layout = layouts_.Of(inst.type);
        const Register area = UseArea(id, 0);
        const Register one = Scratch(1);
        assembler_.MovImmediate(IWidth::k32, one, 1);
        assembler_.Strb(one, area, 0);
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
        assembler_.Ldrb(reg, base, 0);
        FinishInt(id, reg);
        return;
      }
      case ir::Opcode::kOptionalValue: {
        const ir::InstId optional = function_.OperandsOf(inst)[0];
        const abi::Layout& layout = layouts_.Of(TypeOf(optional));
        const Register base = UseArea(optional, 1);
        const Register tag = Scratch(2);
        assembler_.Ldrb(tag, base, 0);
        const Label trap =
            TrapLabel(ir::TrapKind::kUndefinedValue, id,
                      "read the value of an optional that is absent");
        assembler_.Cbz(IWidth::k32, tag, trap);
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
        assembler_.CmpImm(IWidth::k32, source, 0);
        assembler_.Cset(IWidth::k32, reg, Condition::kEq);
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

  // The bits a constant of |type| is held in. The interpreter keeps a
  // narrow integer masked to its width; a register keeps it extended, so
  // that a 32-bit compare answers the question the width asked.
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
    assembler_.BCond(Condition::kHs, trap);

    const Register base = UseArea(operands[0], 3);
    const Register offset = Scratch(2);
    assembler_.MovImmediate(IWidth::k64, offset, stride);
    assembler_.Mul(IWidth::k64, offset, index, offset);
    assembler_.Add(IWidth::k64, base, base, offset);
    if (layouts_.ClassOf(inst.type) == abi::ValueClass::kAggregate) {
      const Register area = UseArea(id, 4);
      CopyBytes(area, 0, base, 0, layouts_.SizeOf(inst.type), 5);
      return;
    }
    LoadScalar(inst.type, id, base, 0);
  }

  void EmitCall(ir::InstId id, const ir::Inst& inst) {
    const bool indirect_callee = inst.opcode == ir::Opcode::kCallIndirect;
    const std::span<const ir::InstId> all = function_.OperandsOf(inst);
    // Through a value the first operand is the function; the rest are the
    // arguments. Loaded before any argument register is written, because
    // the value may well be in one.
    const Register target = kCallScratch;
    if (indirect_callee && !all.empty()) {
      assembler_.Mov(IWidth::k64, kCallScratch, UseInt(all[0], 0));
    }
    const TypeId returned = inst.type;
    const bool indirect = returned.is_valid() &&
                          layouts_.ClassOf(returned) ==
                              abi::ValueClass::kAggregate;
    if (indirect) {
      assembler_.AddImm(IWidth::k64, kIndirectResult, Register::kSp,
                        Info(id).area);
    }

    uint32_t integers = 0;
    uint32_t floats = 0;
    for (size_t index = indirect_callee ? 1 : 0; index < all.size();
         ++index) {
      const ir::InstId operand = all[index];
      const TypeId type = TypeOf(operand);
      switch (layouts_.ClassOf(type)) {
        case abi::ValueClass::kFloat: {
          const FloatRegister source = UseFloat(operand, 0);
          assembler_.FMov(FWidth::k64, kFloatArgumentRegisters[floats++],
                          source);
          break;
        }
        case abi::ValueClass::kAggregate:
          assembler_.AddImm(IWidth::k64, kArgumentRegisters[integers++],
                            Register::kSp, Info(operand).area);
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

    if (indirect_callee) {
      assembler_.Blr(target);
      StoreResult(id, returned, indirect);
      return;
    }
    const uint64_t address = inst.payload.callee < foreign_.size()
                                 ? foreign_[inst.payload.callee]
                                 : 0;
    if (address != 0) {
      // Out of the language: the address came from the host's loader, and
      // it is too far away for a `bl`. The arguments are already where the
      // platform ABI wants them -- which is the one thing this tier gets
      // for free from having followed it.
      assembler_.MovImmediate(IWidth::k64, kCallScratch, address);
      assembler_.Blr(kCallScratch);
    } else {
      calls_.push_back(CallFixup{
          static_cast<uint32_t>(assembler_.words().size() *
                                sizeof(uint32_t)),
          inst.payload.callee});
      // The displacement is filled in once every function has been placed.
      assembler_.BlPlaceholder();
    }

    StoreResult(id, returned, indirect);
  }

  // Where a call's result goes. An aggregate was written through the
  // indirect result register, so there is nothing to move.
  void StoreResult(ir::InstId id, TypeId returned, bool indirect) {
    if (indirect || !returned.is_valid()) {
      return;
    }
    switch (layouts_.ClassOf(returned)) {
      case abi::ValueClass::kFloat: {
        const FloatRegister reg = DestFloat(id, 0);
        assembler_.FMov(FWidth::k64, reg, FloatRegister::kV0);
        FinishFloat(id, reg);
        break;
      }
      case abi::ValueClass::kVoid:
        break;
      default: {
        const Register reg = DestInt(id, 0);
        assembler_.Mov(IWidth::k64, reg, Register::kX0);
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
              assembler_.FMov(FWidth::k64, FloatRegister::kV0,
                              UseFloat(value, 0));
              break;
            case abi::ValueClass::kAggregate: {
              const Register source = UseArea(value, 1);
              CopyBytes(kIndirectResult, 0, source, 0,
                        layouts_.SizeOf(type), 2);
              break;
            }
            case abi::ValueClass::kVoid:
              break;
            default:
              assembler_.Mov(IWidth::k64, Register::kX0, UseInt(value, 0));
              break;
          }
        }
        assembler_.B(epilogue_);
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
        const Register condition =
            UseInt(function_.OperandsOf(inst)[0], 0);
        // A branch carries no arguments: lowering passes block parameters
        // through a jump, so there is nothing to move here.
        if (targets[1] == next_block_) {
          assembler_.Cbnz(IWidth::k32, condition, labels_[targets[0].index]);
          JumpTo(targets[1]);
          return;
        }
        assembler_.Cbz(IWidth::k32, condition, labels_[targets[1].index]);
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
          assembler_.BCond(Condition::kEq, labels_[targets[i].index]);
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
    assembler_.B(labels_[target.index]);
  }

  // The values a jump passes become the target's parameters. Direct moves
  // where nothing is overwritten before it is read, and a trip through the
  // frame where something would be -- which is only the case when the
  // values going around a loop trade places.
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
        const FloatRegister source = UseFloat(from, 0);
        const FloatRegister destination = DestFloat(to, 1);
        assembler_.FMov(FWidth::k64, destination, source);
        FinishFloat(to, destination);
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
    const int32_t offset = static_cast<int32_t>(swap_base_ + index * 8);
    const TypeId type = TypeOf(value);
    switch (layouts_.ClassOf(type)) {
      case abi::ValueClass::kFloat:
        assembler_.StrFloat(FWidth::k64, UseFloat(value, 0), Register::kSp,
                            offset);
        break;
      case abi::ValueClass::kAggregate:
        // The address is what an aggregate value is, and an area never
        // moves, so there is nothing to trade.
        break;
      case abi::ValueClass::kVoid:
        break;
      default:
        assembler_.Str(IWidth::k64, UseInt(value, 0), Register::kSp,
                       offset);
        break;
    }
  }

  void LoadFromSwap(ir::InstId value, uint32_t index) {
    const int32_t offset = static_cast<int32_t>(swap_base_ + index * 8);
    const TypeId type = TypeOf(value);
    switch (layouts_.ClassOf(type)) {
      case abi::ValueClass::kFloat: {
        const FloatRegister reg = DestFloat(value, 0);
        assembler_.LdrFloat(FWidth::k64, reg, Register::kSp, offset);
        FinishFloat(value, reg);
        break;
      }
      case abi::ValueClass::kAggregate:
      case abi::ValueClass::kVoid:
        break;
      default: {
        const Register reg = DestInt(value, 0);
        assembler_.Ldr(IWidth::k64, reg, Register::kSp, offset);
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
    const bool from_float =
        layouts_.ClassOf(from) == abi::ValueClass::kFloat;
    const bool to_float = layouts_.ClassOf(to) == abi::ValueClass::kFloat;

    if (from_float && to_float) {
      const FloatRegister value = UseFloat(source, 1);
      const FloatRegister reg = DestFloat(id, 0);
      if (FloatWidthOf(from) == FloatWidthOf(to)) {
        assembler_.FMov(FloatWidthOf(to), reg, value);
      } else {
        assembler_.FCvt(FloatWidthOf(to), FloatWidthOf(from), reg, value);
      }
      FinishFloat(id, reg);
      return;
    }

    if (!from_float && to_float) {
      const Register value = UseInt(source, 1);
      const FloatRegister reg = DestFloat(id, 0);
      const IWidth width = IntWidthOf(from);
      if (layouts_.IsSignedInteger(from)) {
        assembler_.Scvtf(FloatWidthOf(to), reg, width, value);
      } else {
        assembler_.Ucvtf(FloatWidthOf(to), reg, width, value);
      }
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
    // rather than keeping the low bits: `as` is a conversion, and overflow
    // traps everywhere else in this language.
    const Register value = UseInt(source, 1);
    const Register wide = Scratch(2);
    Extend64(value, from, wide);
    const Register reg = DestInt(id, 0);
    const uint32_t bits = layouts_.BitsOf(to);
    if (bits >= 64) {
      assembler_.Mov(IWidth::k64, reg, wide);
      FinishInt(id, reg);
      return;
    }
    const Register narrowed = Scratch(3);
    Canonicalize(wide, to, narrowed);
    assembler_.Cmp(IWidth::k64, narrowed, wide);
    const Label trap =
        TrapLabel(ir::TrapKind::kOverflow, id,
                  "the value does not fit the integer it was cast to");
    assembler_.BCond(Condition::kNe, trap);
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
      assembler_.Sxtw(destination, value);
    } else {
      assembler_.Uxtw(destination, value);
    }
  }

  // The canonical form of the low bits of |value| at |type|.
  void Canonicalize(Register value, TypeId type, Register destination) {
    const uint32_t bits = layouts_.BitsOf(type);
    const bool is_signed = layouts_.IsSignedInteger(type);
    switch (bits) {
      case 8:
        if (is_signed) {
          assembler_.Sxtb(IWidth::k64, destination, value);
        } else {
          assembler_.Uxtb(destination, value);
        }
        return;
      case 16:
        if (is_signed) {
          assembler_.Sxth(IWidth::k64, destination, value);
        } else {
          assembler_.Uxth(destination, value);
        }
        return;
      case 32:
        if (is_signed) {
          assembler_.Sxtw(destination, value);
        } else {
          assembler_.Uxtw(destination, value);
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
    const FloatRegister value = UseFloat(source, 1);
    const FWidth width = FloatWidthOf(from);
    const bool is_signed = layouts_.IsSignedInteger(to);
    const Label trap =
        TrapLabel(ir::TrapKind::kOverflow, id,
                  "the value does not fit the integer it was cast to");

    // Not a number converts to nothing, so it is the first thing ruled out.
    assembler_.FCmp(width, value, value);
    assembler_.BCond(Condition::kVs, trap);

    const Register raw = Scratch(2);
    if (is_signed) {
      assembler_.Fcvtzs(IWidth::k64, raw, width, value);
    } else {
      assembler_.Fcvtzu(IWidth::k64, raw, width, value);
    }

    const uint32_t bits = layouts_.BitsOf(to);
    if (bits >= 64) {
      // The conversion saturates rather than faulting, so a 64-bit target
      // has to have its range checked against the float itself. Both
      // bounds are exact powers of two, which every float width holds
      // without rounding, so the comparison is exact at the boundary --
      // and the boundary is where this is easy to get wrong.
      const FloatRegister limit = FloatScratch(2);
      const Register pattern = Scratch(3);
      MaterializeFloat(width, limit, pattern,
                       is_signed ? 9223372036854775808.0
                                 : 18446744073709551616.0);
      assembler_.FCmp(width, value, limit);
      assembler_.BCond(Condition::kGe, trap);
      MaterializeFloat(width, limit, pattern,
                       is_signed ? -9223372036854775808.0 : -1.0);
      assembler_.FCmp(width, value, limit);
      // The most negative value is in range; anything below it is not.
      // Truncation is why the unsigned bound is minus one rather than
      // zero: `-0.5 as u32` is zero, not a fault.
      assembler_.BCond(is_signed ? Condition::kMi : Condition::kLs, trap);
      const Register whole = DestInt(id, 0);
      assembler_.Mov(IWidth::k64, whole, raw);
      FinishInt(id, whole);
      return;
    }

    const Register narrowed = Scratch(3);
    Canonicalize(raw, to, narrowed);
    assembler_.Cmp(IWidth::k64, narrowed, raw);
    assembler_.BCond(Condition::kNe, trap);
    const Register reg = DestInt(id, 0);
    assembler_.Mov(IWidth::k64, reg, narrowed);
    FinishInt(id, reg);
    (void)inst;
  }

  void MaterializeFloat(FWidth width,
                        FloatRegister destination,
                        Register scratch,
                        double value) {
    if (width == FWidth::k64) {
      uint64_t pattern = 0;
      std::memcpy(&pattern, &value, sizeof(pattern));
      assembler_.MovImmediate(IWidth::k64, scratch, pattern);
      assembler_.FMovFromGeneral(FWidth::k64, destination, scratch);
      return;
    }
    uint32_t pattern = 0;
    const float narrow = static_cast<float>(value);
    std::memcpy(&pattern, &narrow, sizeof(pattern));
    assembler_.MovImmediate(IWidth::k32, scratch, pattern);
    assembler_.FMovFromGeneral(FWidth::k32, destination, scratch);
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
    const FloatRegister left = UseFloat(operands[0], 1);
    const FloatRegister right =
        operands.size() > 1 ? UseFloat(operands[1], 2) : left;

    switch (inst.opcode) {
      case ir::Opcode::kAdd:
      case ir::Opcode::kSub:
      case ir::Opcode::kMul:
      case ir::Opcode::kDiv:
      case ir::Opcode::kNeg: {
        const FloatRegister reg = DestFloat(id, 0);
        switch (inst.opcode) {
          case ir::Opcode::kAdd:
            assembler_.FAdd(width, reg, left, right);
            break;
          case ir::Opcode::kSub:
            assembler_.FSub(width, reg, left, right);
            break;
          case ir::Opcode::kMul:
            assembler_.FMul(width, reg, left, right);
            break;
          case ir::Opcode::kDiv:
            // Division by zero is an infinity in IEEE-754, which is
            // defined and not a fault.
            assembler_.FDiv(width, reg, left, right);
            break;
          default:
            assembler_.FNeg(width, reg, left);
            break;
        }
        FinishFloat(id, reg);
        return;
      }
      default:
        break;
    }

    // A comparison. The unordered case falls out of the condition codes the
    // same way it falls out of C++: every ordered test is false for a
    // comparison with a value that is not a number.
    Condition condition = Condition::kEq;
    switch (inst.opcode) {
      case ir::Opcode::kEq:
        condition = Condition::kEq;
        break;
      case ir::Opcode::kNe:
        condition = Condition::kNe;
        break;
      case ir::Opcode::kLt:
        condition = Condition::kMi;
        break;
      case ir::Opcode::kLe:
        condition = Condition::kLs;
        break;
      case ir::Opcode::kGt:
        condition = Condition::kGt;
        break;
      case ir::Opcode::kGe:
        condition = Condition::kGe;
        break;
      default:
        TrapNow(ir::TrapKind::kUndefinedValue, id, "has no execution");
        return;
    }
    const Register reg = DestInt(id, 0);
    assembler_.FCmp(width, left, right);
    assembler_.Cset(IWidth::k32, reg, condition);
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

    switch (inst.opcode) {
      case ir::Opcode::kAdd:
      case ir::Opcode::kSub: {
        const Register reg = DestInt(id, 0);
        const bool is_add = inst.opcode == ir::Opcode::kAdd;
        if (bits >= 32) {
          if (is_add) {
            assembler_.Adds(width, reg, left, right);
          } else {
            assembler_.Subs(width, reg, left, right);
          }
          const Label trap = TrapLabel(
              ir::TrapKind::kOverflow, id,
              std::string(ir::OpcodeMnemonic(inst.opcode)) + " overflowed");
          assembler_.BCond(is_signed ? Condition::kVs
                                     : (is_add ? Condition::kHs
                                               : Condition::kLo),
                           trap);
          FinishInt(id, reg);
          return;
        }
        // A narrower width has no flag of its own: do the arithmetic wide
        // and ask whether the answer still fits.
        const Register raw = Scratch(3);
        if (is_add) {
          assembler_.Add(IWidth::k32, raw, left, right);
        } else {
          assembler_.Sub(IWidth::k32, raw, left, right);
        }
        CheckFits(id, raw, operand_type, reg, inst.opcode);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kMul: {
        const Register reg = DestInt(id, 0);
        if (bits >= 64) {
          const Register high = Scratch(3);
          assembler_.Mul(IWidth::k64, reg, left, right);
          const Label trap =
              TrapLabel(ir::TrapKind::kOverflow, id, "mul overflowed");
          if (is_signed) {
            assembler_.Smulh(high, left, right);
            const Register sign = Scratch(4);
            assembler_.Asr(IWidth::k64, sign, reg, 63);
            assembler_.Cmp(IWidth::k64, sign, high);
            assembler_.BCond(Condition::kNe, trap);
          } else {
            assembler_.Umulh(high, left, right);
            assembler_.Cbnz(IWidth::k64, high, trap);
          }
          FinishInt(id, reg);
          return;
        }
        // Up to 32 bits the product of two extended operands cannot wrap a
        // 64-bit register, so one wide multiply answers both questions.
        const Register wide_left = Scratch(3);
        const Register wide_right = Scratch(4);
        Extend64(left, operand_type, wide_left);
        Extend64(right, operand_type, wide_right);
        const Register raw = Scratch(5);
        assembler_.Mul(IWidth::k64, raw, wide_left, wide_right);
        CheckFits(id, raw, operand_type, reg, inst.opcode);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kDiv:
      case ir::Opcode::kRem: {
        const Label by_zero = TrapLabel(ir::TrapKind::kDivisionByZero, id,
                                        "divided by zero");
        assembler_.Cbz(width, right, by_zero);
        if (is_signed) {
          // The one signed division that overflows: the most negative
          // value divided by minus one.
          const Label skip = assembler_.CreateLabel();
          const Register minus_one = Scratch(3);
          assembler_.MovImmediate(width, minus_one,
                                  width == IWidth::k64
                                      ? ~uint64_t{0}
                                      : 0xFFFFFFFFu);
          assembler_.Cmp(width, right, minus_one);
          assembler_.BCond(Condition::kNe, skip);
          const Register lowest = Scratch(3);
          assembler_.MovImmediate(width, lowest, LowestOf(operand_type));
          assembler_.Cmp(width, left, lowest);
          const Label overflow = TrapLabel(ir::TrapKind::kOverflow, id,
                                           "the quotient does not fit");
          assembler_.BCond(Condition::kEq, overflow);
          assembler_.Bind(skip);
        }
        const Register reg = DestInt(id, 0);
        const Register quotient =
            inst.opcode == ir::Opcode::kRem ? Scratch(3) : reg;
        if (is_signed) {
          assembler_.Sdiv(width, quotient, left, right);
        } else {
          assembler_.Udiv(width, quotient, left, right);
        }
        if (inst.opcode == ir::Opcode::kRem) {
          assembler_.Msub(width, reg, quotient, right, left);
        }
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kNeg: {
        const Register reg = DestInt(id, 0);
        if (bits >= 32) {
          assembler_.Subs(width, reg, Register::kZr, left);
          const Label trap = TrapLabel(ir::TrapKind::kOverflow, id,
                                       "negation overflowed");
          assembler_.BCond(Condition::kVs, trap);
          FinishInt(id, reg);
          return;
        }
        const Register raw = Scratch(3);
        assembler_.Sub(IWidth::k32, raw, Register::kZr, left);
        CheckFits(id, raw, operand_type, reg, inst.opcode);
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kBitAnd:
      case ir::Opcode::kBitOr:
      case ir::Opcode::kBitXor: {
        const Register reg = DestInt(id, 0);
        if (inst.opcode == ir::Opcode::kBitAnd) {
          assembler_.And(width, reg, left, right);
        } else if (inst.opcode == ir::Opcode::kBitOr) {
          assembler_.Orr(width, reg, left, right);
        } else {
          assembler_.Eor(width, reg, left, right);
        }
        FinishInt(id, reg);
        return;
      }
      case ir::Opcode::kBitNot: {
        const Register reg = DestInt(id, 0);
        if (bits >= 32) {
          assembler_.Mvn(width, reg, left);
        } else {
          const Register raw = Scratch(3);
          assembler_.Mvn(IWidth::k32, raw, left);
          Canonicalize(raw, operand_type, reg);
        }
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kShl:
      case ir::Opcode::kShr: {
        const Register limit = Scratch(3);
        // Compared at the operand's own width: a 64-bit shift amount whose
        // low half looks small is still too large, and the interpreter
        // reads the whole value.
        assembler_.MovImmediate(width, limit, bits);
        assembler_.Cmp(width, right, limit);
        const Label trap = TrapLabel(ir::TrapKind::kOverflow, id,
                                     "shifted by more than the width");
        assembler_.BCond(Condition::kHs, trap);
        const Register reg = DestInt(id, 0);
        const Register raw = bits >= 32 ? reg : Scratch(4);
        if (inst.opcode == ir::Opcode::kShl) {
          assembler_.Lslv(width, raw, left, right);
        } else if (is_signed) {
          assembler_.Asrv(width, raw, left, right);
        } else {
          assembler_.Lsrv(width, raw, left, right);
        }
        if (bits < 32) {
          // A shift truncates rather than faulting, the same way the
          // interpreter masks to the width.
          Canonicalize(raw, operand_type, reg);
        }
        FinishInt(id, reg);
        return;
      }

      case ir::Opcode::kEq:
      case ir::Opcode::kNe:
      case ir::Opcode::kLt:
      case ir::Opcode::kLe:
      case ir::Opcode::kGt:
      case ir::Opcode::kGe: {
        Condition condition = Condition::kEq;
        switch (inst.opcode) {
          case ir::Opcode::kEq:
            condition = Condition::kEq;
            break;
          case ir::Opcode::kNe:
            condition = Condition::kNe;
            break;
          case ir::Opcode::kLt:
            condition = is_signed ? Condition::kLt : Condition::kLo;
            break;
          case ir::Opcode::kLe:
            condition = is_signed ? Condition::kLe : Condition::kLs;
            break;
          case ir::Opcode::kGt:
            condition = is_signed ? Condition::kGt : Condition::kHi;
            break;
          default:
            condition = is_signed ? Condition::kGe : Condition::kHs;
            break;
        }
        const Register reg = DestInt(id, 0);
        assembler_.Cmp(width, left, right);
        assembler_.Cset(IWidth::k32, reg, condition);
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
    assembler_.BCond(Condition::kNe, trap);
    assembler_.Mov(IWidth::k64, destination, narrowed);
  }

  uint64_t LowestOf(TypeId type) const {
    const uint32_t bits = layouts_.BitsOf(type);
    if (bits >= 64) {
      return uint64_t{1} << 63;
    }
    const uint64_t value = uint64_t{1} << (bits - 1);
    // The canonical form of the most negative value of a narrow type.
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
  const ir::Function& function_;
  const std::vector<bool>& compilable_;
  const std::vector<uint64_t>& foreign_;
  const std::vector<std::pair<uint64_t, uint64_t>>& strings_;
  uint32_t trap_site_base_ = 0;

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
  uint32_t saved_floats_ = 0;
  uint32_t spill_base_ = 0;
  uint32_t swap_base_ = 0;
  uint32_t frame_bytes_ = 0;
  // Every `alloca`'s area, which the prologue zeroes.
  std::vector<std::pair<uint32_t, uint32_t>> slots_;

  std::vector<TrapSite> traps_;
  std::vector<PendingTrap> pending_traps_;
  std::vector<CallFixup> calls_;
  std::vector<AddressFixup> addresses_;
};

}  // namespace

namespace arm64 {

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

}  // namespace arm64

}  // namespace codegen
}  // namespace wolang
