// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/arm64/assembler.h"

#include "wolang/base/check.h"

namespace wolang {
namespace codegen {
namespace arm64 {
namespace {

constexpr uint32_t kUnbound = 0xFFFFFFFFu;

uint32_t Num(Register reg) {
  return static_cast<uint32_t>(reg) & 31u;
}
uint32_t Num(FloatRegister reg) {
  return static_cast<uint32_t>(reg) & 31u;
}
// The `sf` bit, which is what makes an instruction operate on 64 bits.
uint32_t Sf(Width width) {
  return width == Width::k64 ? 1u << 31 : 0u;
}
// The `ftype` field: 00 for single, 01 for double.
uint32_t Ftype(FloatWidth width) {
  return width == FloatWidth::k64 ? 1u << 22 : 0u;
}
uint32_t Bits(Width width) {
  return width == Width::k64 ? 64u : 32u;
}

}  // namespace

Condition Invert(Condition condition) {
  // The architecture pairs conditions so that flipping the low bit inverts
  // the test. `al` has no opposite and stays itself.
  if (condition == Condition::kAl) {
    return condition;
  }
  return static_cast<Condition>(static_cast<uint8_t>(condition) ^ 1u);
}

// --------------------------------------------------------------- labels ---

Label Assembler::CreateLabel() {
  labels_.push_back(kUnbound);
  return Label{static_cast<uint32_t>(labels_.size() - 1)};
}

void Assembler::Bind(Label label) {
  WOLANG_CHECK(label.is_valid() && label.index < labels_.size());
  WOLANG_CHECK(labels_[label.index] == kUnbound);
  labels_[label.index] = static_cast<uint32_t>(words_.size());
}

uint32_t Assembler::OffsetOf(Label label) const {
  WOLANG_CHECK(label.is_valid() && label.index < labels_.size());
  WOLANG_CHECK(labels_[label.index] != kUnbound);
  return labels_[label.index] * static_cast<uint32_t>(sizeof(uint32_t));
}

void Assembler::Resolve() {
  for (const Fixup& fixup : fixups_) {
    WOLANG_CHECK(fixup.label.index < labels_.size());
    const uint32_t target = labels_[fixup.label.index];
    // A branch to a label nothing bound is a bug in the emitter: the
    // program being compiled cannot cause it.
    WOLANG_CHECK(target != kUnbound);
    const int32_t distance =
        static_cast<int32_t>(target) - static_cast<int32_t>(fixup.word);
    uint32_t& word = words_[fixup.word];
    if (fixup.kind == FixupKind::kBranch26) {
      WOLANG_CHECK(distance >= -(1 << 25) && distance < (1 << 25));
      word |= static_cast<uint32_t>(distance) & 0x03FFFFFFu;
    } else {
      WOLANG_CHECK(distance >= -(1 << 18) && distance < (1 << 18));
      word |= (static_cast<uint32_t>(distance) & 0x7FFFFu) << 5;
    }
  }
  fixups_.clear();
}

void Assembler::EmitBranch(uint32_t word, Label label, FixupKind kind) {
  fixups_.push_back(
      Fixup{static_cast<uint32_t>(words_.size()), label, kind});
  Emit(word);
}

// ---------------------------------------------------------------- moves ---

void Assembler::Movz(Width width,
                     Register rd,
                     uint16_t value,
                     unsigned shift) {
  WOLANG_CHECK(shift % 16 == 0 && shift < Bits(width));
  Emit(0x52800000u | Sf(width) | ((shift / 16) << 21) |
       (static_cast<uint32_t>(value) << 5) | Num(rd));
}

void Assembler::Movk(Width width,
                     Register rd,
                     uint16_t value,
                     unsigned shift) {
  WOLANG_CHECK(shift % 16 == 0 && shift < Bits(width));
  Emit(0x72800000u | Sf(width) | ((shift / 16) << 21) |
       (static_cast<uint32_t>(value) << 5) | Num(rd));
}

void Assembler::Movn(Width width,
                     Register rd,
                     uint16_t value,
                     unsigned shift) {
  WOLANG_CHECK(shift % 16 == 0 && shift < Bits(width));
  Emit(0x12800000u | Sf(width) | ((shift / 16) << 21) |
       (static_cast<uint32_t>(value) << 5) | Num(rd));
}

void Assembler::Mov(Width width, Register rd, Register rn) {
  Orr(width, rd, Register::kZr, rn);
}

void Assembler::MovImmediate(Width width, Register rd, uint64_t value) {
  const unsigned chunks = width == Width::k64 ? 4u : 2u;
  if (width == Width::k32) {
    value &= 0xFFFFFFFFu;
  }
  const uint64_t inverted =
      width == Width::k32 ? (~value) & 0xFFFFFFFFu : ~value;

  unsigned set = 0;
  unsigned clear = 0;
  for (unsigned i = 0; i < chunks; ++i) {
    set += ((value >> (16 * i)) & 0xFFFFu) != 0 ? 1 : 0;
    clear += ((inverted >> (16 * i)) & 0xFFFFu) != 0 ? 1 : 0;
  }

  if (set == 0) {
    Movz(width, rd, 0, 0);
    return;
  }
  // One `movn` covers every value whose inverse fits in a single chunk,
  // which is what makes a small negative number one word rather than four.
  if (clear <= 1) {
    for (unsigned i = 0; i < chunks; ++i) {
      const auto part = static_cast<uint16_t>((inverted >> (16 * i)) & 0xFFFF);
      if (part != 0 || clear == 0) {
        Movn(width, rd, part, 16 * i);
        return;
      }
    }
  }

  bool first = true;
  for (unsigned i = 0; i < chunks; ++i) {
    const auto part = static_cast<uint16_t>((value >> (16 * i)) & 0xFFFF);
    if (part == 0) {
      continue;
    }
    if (first) {
      Movz(width, rd, part, 16 * i);
      first = false;
    } else {
      Movk(width, rd, part, 16 * i);
    }
  }
}

// ----------------------------------------------------------- arithmetic ---

namespace {

uint32_t ShiftedRegister(uint32_t base,
                         Width width,
                         Register rd,
                         Register rn,
                         Register rm) {
  return base | Sf(width) | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd);
}

}  // namespace

void Assembler::Add(Width width, Register rd, Register rn, Register rm) {
  Emit(ShiftedRegister(0x0B000000u, width, rd, rn, rm));
}
void Assembler::Sub(Width width, Register rd, Register rn, Register rm) {
  Emit(ShiftedRegister(0x4B000000u, width, rd, rn, rm));
}
void Assembler::Adds(Width width, Register rd, Register rn, Register rm) {
  Emit(ShiftedRegister(0x2B000000u, width, rd, rn, rm));
}
void Assembler::Subs(Width width, Register rd, Register rn, Register rm) {
  Emit(ShiftedRegister(0x6B000000u, width, rd, rn, rm));
}

void Assembler::AddImm(Width width,
                       Register rd,
                       Register rn,
                       uint32_t value) {
  WOLANG_CHECK(value < 4096);
  Emit(0x11000000u | Sf(width) | (value << 10) | (Num(rn) << 5) | Num(rd));
}
void Assembler::SubImm(Width width,
                       Register rd,
                       Register rn,
                       uint32_t value) {
  WOLANG_CHECK(value < 4096);
  Emit(0x51000000u | Sf(width) | (value << 10) | (Num(rn) << 5) | Num(rd));
}
void Assembler::SubsImm(Width width,
                        Register rd,
                        Register rn,
                        uint32_t value) {
  WOLANG_CHECK(value < 4096);
  Emit(0x71000000u | Sf(width) | (value << 10) | (Num(rn) << 5) | Num(rd));
}

void Assembler::Mul(Width width, Register rd, Register rn, Register rm) {
  // `madd rd, rn, rm, zr`.
  Emit(0x1B000000u | Sf(width) | (Num(rm) << 16) | (31u << 10) |
       (Num(rn) << 5) | Num(rd));
}
void Assembler::Msub(Width width,
                     Register rd,
                     Register rn,
                     Register rm,
                     Register ra) {
  Emit(0x1B008000u | Sf(width) | (Num(rm) << 16) | (Num(ra) << 10) |
       (Num(rn) << 5) | Num(rd));
}
void Assembler::Sdiv(Width width, Register rd, Register rn, Register rm) {
  Emit(0x1AC00C00u | Sf(width) | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Udiv(Width width, Register rd, Register rn, Register rm) {
  Emit(0x1AC00800u | Sf(width) | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Smulh(Register rd, Register rn, Register rm) {
  Emit(0x9B407C00u | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Umulh(Register rd, Register rn, Register rm) {
  Emit(0x9BC07C00u | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd));
}

void Assembler::And(Width width, Register rd, Register rn, Register rm) {
  Emit(ShiftedRegister(0x0A000000u, width, rd, rn, rm));
}
void Assembler::Orr(Width width, Register rd, Register rn, Register rm) {
  Emit(ShiftedRegister(0x2A000000u, width, rd, rn, rm));
}
void Assembler::Eor(Width width, Register rd, Register rn, Register rm) {
  Emit(ShiftedRegister(0x4A000000u, width, rd, rn, rm));
}
void Assembler::Mvn(Width width, Register rd, Register rn) {
  // `orn rd, zr, rn`.
  Emit(0x2A200000u | Sf(width) | (Num(rn) << 16) | (31u << 5) | Num(rd));
}

void Assembler::Lslv(Width width, Register rd, Register rn, Register rm) {
  Emit(0x1AC02000u | Sf(width) | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Lsrv(Width width, Register rd, Register rn, Register rm) {
  Emit(0x1AC02400u | Sf(width) | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Asrv(Width width, Register rd, Register rn, Register rm) {
  Emit(0x1AC02800u | Sf(width) | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd));
}

namespace {

// The bit field moves every immediate shift and extension is written as.
uint32_t BitfieldMove(bool is_signed,
                      Width width,
                      Register rd,
                      Register rn,
                      unsigned immr,
                      unsigned imms) {
  const uint32_t base = is_signed ? 0x13000000u : 0x53000000u;
  const uint32_t n = width == Width::k64 ? 1u << 22 : 0u;
  return base | Sf(width) | n | (immr << 16) | (imms << 10) |
         (Num(rn) << 5) | Num(rd);
}

}  // namespace

void Assembler::Lsl(Width width,
                    Register rd,
                    Register rn,
                    unsigned amount) {
  const unsigned bits = Bits(width);
  WOLANG_CHECK(amount < bits);
  Emit(BitfieldMove(false, width, rd, rn, (bits - amount) % bits,
                    bits - 1 - amount));
}
void Assembler::Lsr(Width width,
                    Register rd,
                    Register rn,
                    unsigned amount) {
  const unsigned bits = Bits(width);
  WOLANG_CHECK(amount < bits);
  Emit(BitfieldMove(false, width, rd, rn, amount, bits - 1));
}
void Assembler::Asr(Width width,
                    Register rd,
                    Register rn,
                    unsigned amount) {
  const unsigned bits = Bits(width);
  WOLANG_CHECK(amount < bits);
  Emit(BitfieldMove(true, width, rd, rn, amount, bits - 1));
}

void Assembler::Sxtb(Width width, Register rd, Register rn) {
  Emit(BitfieldMove(true, width, rd, rn, 0, 7));
}
void Assembler::Sxth(Width width, Register rd, Register rn) {
  Emit(BitfieldMove(true, width, rd, rn, 0, 15));
}
void Assembler::Sxtw(Register rd, Register rn) {
  Emit(BitfieldMove(true, Width::k64, rd, rn, 0, 31));
}
void Assembler::Uxtb(Register rd, Register rn) {
  Emit(BitfieldMove(false, Width::k32, rd, rn, 0, 7));
}
void Assembler::Uxth(Register rd, Register rn) {
  Emit(BitfieldMove(false, Width::k32, rd, rn, 0, 15));
}
void Assembler::Uxtw(Register rd, Register rn) {
  // A 32-bit move already clears the top half.
  Mov(Width::k32, rd, rn);
}

// ---------------------------------------------------------- comparisons ---

void Assembler::Cmp(Width width, Register rn, Register rm) {
  Subs(width, Register::kZr, rn, rm);
}
void Assembler::CmpImm(Width width, Register rn, uint32_t value) {
  SubsImm(width, Register::kZr, rn, value);
}
void Assembler::Cset(Width width, Register rd, Condition condition) {
  // `csinc rd, zr, zr, invert(condition)`.
  Emit(0x1A800400u | Sf(width) | (31u << 16) |
       (static_cast<uint32_t>(Invert(condition)) << 12) | (31u << 5) |
       Num(rd));
}
void Assembler::Csel(Width width,
                     Register rd,
                     Register rn,
                     Register rm,
                     Condition condition) {
  Emit(0x1A800000u | Sf(width) | (Num(rm) << 16) |
       (static_cast<uint32_t>(condition) << 12) | (Num(rn) << 5) | Num(rd));
}

// --------------------------------------------------------------- memory ---

void Assembler::LoadStore(uint32_t scaled,
                          uint32_t unscaled,
                          unsigned size_log2,
                          uint32_t rt,
                          Register base,
                          int32_t offset) {
  const int32_t scale = 1 << size_log2;
  if (offset >= 0 && offset % scale == 0 && (offset / scale) < 4096) {
    Emit(scaled | (static_cast<uint32_t>(offset / scale) << 10) |
         (Num(base) << 5) | rt);
    return;
  }
  // The unscaled form reaches a small negative offset, which a frame laid
  // out below the frame pointer needs.
  WOLANG_CHECK(offset >= -256 && offset <= 255);
  Emit(unscaled | ((static_cast<uint32_t>(offset) & 0x1FFu) << 12) |
       (Num(base) << 5) | rt);
  (void)scaled;
}

namespace {

// The unscaled form of a load or store differs from the scaled one in one
// bit, which is what makes "the offset does not fit" a local decision.
uint32_t Unscaled(uint32_t scaled) {
  return scaled & ~(1u << 24);
}

}  // namespace

void Assembler::Ldr(Width width,
                    Register rt,
                    Register base,
                    int32_t offset) {
  const uint32_t op = width == Width::k64 ? 0xF9400000u : 0xB9400000u;
  LoadStore(op, Unscaled(op), width == Width::k64 ? 3 : 2, Num(rt), base,
            offset);
}
void Assembler::Str(Width width,
                    Register rt,
                    Register base,
                    int32_t offset) {
  const uint32_t op = width == Width::k64 ? 0xF9000000u : 0xB9000000u;
  LoadStore(op, Unscaled(op), width == Width::k64 ? 3 : 2, Num(rt), base,
            offset);
}
void Assembler::Ldrb(Register rt, Register base, int32_t offset) {
  LoadStore(0x39400000u, Unscaled(0x39400000u), 0, Num(rt), base, offset);
}
void Assembler::Ldrsb(Width width,
                      Register rt,
                      Register base,
                      int32_t offset) {
  const uint32_t op = width == Width::k64 ? 0x39800000u : 0x39C00000u;
  LoadStore(op, Unscaled(op), 0, Num(rt), base, offset);
}
void Assembler::Strb(Register rt, Register base, int32_t offset) {
  LoadStore(0x39000000u, Unscaled(0x39000000u), 0, Num(rt), base, offset);
}
void Assembler::Ldrh(Register rt, Register base, int32_t offset) {
  LoadStore(0x79400000u, Unscaled(0x79400000u), 1, Num(rt), base, offset);
}
void Assembler::Ldrsh(Width width,
                      Register rt,
                      Register base,
                      int32_t offset) {
  const uint32_t op = width == Width::k64 ? 0x79800000u : 0x79C00000u;
  LoadStore(op, Unscaled(op), 1, Num(rt), base, offset);
}
void Assembler::Strh(Register rt, Register base, int32_t offset) {
  LoadStore(0x79000000u, Unscaled(0x79000000u), 1, Num(rt), base, offset);
}
void Assembler::Ldrsw(Register rt, Register base, int32_t offset) {
  LoadStore(0xB9800000u, Unscaled(0xB9800000u), 2, Num(rt), base, offset);
}

void Assembler::LdrFloat(FloatWidth width,
                         FloatRegister rt,
                         Register base,
                         int32_t offset) {
  const uint32_t op = width == FloatWidth::k64 ? 0xFD400000u : 0xBD400000u;
  LoadStore(op, Unscaled(op), width == FloatWidth::k64 ? 3 : 2, Num(rt),
            base, offset);
}
void Assembler::StrFloat(FloatWidth width,
                         FloatRegister rt,
                         Register base,
                         int32_t offset) {
  const uint32_t op = width == FloatWidth::k64 ? 0xFD000000u : 0xBD000000u;
  LoadStore(op, Unscaled(op), width == FloatWidth::k64 ? 3 : 2, Num(rt),
            base, offset);
}

namespace {

uint32_t Pair(uint32_t base,
              Register rt1,
              Register rt2,
              Register rn,
              int32_t offset) {
  WOLANG_CHECK(offset % 8 == 0);
  const int32_t imm7 = offset / 8;
  WOLANG_CHECK(imm7 >= -64 && imm7 < 64);
  return base | ((static_cast<uint32_t>(imm7) & 0x7Fu) << 15) |
         (Num(rt2) << 10) | (Num(rn) << 5) | Num(rt1);
}

}  // namespace

void Assembler::StpPreIndex(Register rt1,
                            Register rt2,
                            Register base,
                            int32_t offset) {
  Emit(Pair(0xA9800000u, rt1, rt2, base, offset));
}
void Assembler::LdpPostIndex(Register rt1,
                             Register rt2,
                             Register base,
                             int32_t offset) {
  Emit(Pair(0xA8C00000u, rt1, rt2, base, offset));
}
void Assembler::Stp(Register rt1,
                    Register rt2,
                    Register base,
                    int32_t offset) {
  Emit(Pair(0xA9000000u, rt1, rt2, base, offset));
}
void Assembler::Ldp(Register rt1,
                    Register rt2,
                    Register base,
                    int32_t offset) {
  Emit(Pair(0xA9400000u, rt1, rt2, base, offset));
}

// ------------------------------------------------------------- branches ---

void Assembler::B(Label label) {
  EmitBranch(0x14000000u, label, FixupKind::kBranch26);
}
void Assembler::BCond(Condition condition, Label label) {
  EmitBranch(0x54000000u | static_cast<uint32_t>(condition), label,
             FixupKind::kBranch19);
}
void Assembler::Cbz(Width width, Register rn, Label label) {
  EmitBranch(0x34000000u | Sf(width) | Num(rn), label,
             FixupKind::kBranch19);
}
void Assembler::Cbnz(Width width, Register rn, Label label) {
  EmitBranch(0x35000000u | Sf(width) | Num(rn), label,
             FixupKind::kBranch19);
}
void Assembler::Br(Register rn) {
  Emit(0xD61F0000u | (Num(rn) << 5));
}
void Assembler::Blr(Register rn) {
  Emit(0xD63F0000u | (Num(rn) << 5));
}
void Assembler::Ret() {
  Emit(0xD65F03C0u);
}
void Assembler::Brk(uint16_t reason) {
  Emit(0xD4200000u | (static_cast<uint32_t>(reason) << 5));
}
void Assembler::Nop() {
  Emit(0xD503201Fu);
}

// ------------------------------------------------------ floating point ---

namespace {

uint32_t FloatBinary(uint32_t base,
                     FloatWidth width,
                     FloatRegister rd,
                     FloatRegister rn,
                     FloatRegister rm) {
  return base | Ftype(width) | (Num(rm) << 16) | (Num(rn) << 5) | Num(rd);
}

}  // namespace

void Assembler::FAdd(FloatWidth width,
                     FloatRegister rd,
                     FloatRegister rn,
                     FloatRegister rm) {
  Emit(FloatBinary(0x1E202800u, width, rd, rn, rm));
}
void Assembler::FSub(FloatWidth width,
                     FloatRegister rd,
                     FloatRegister rn,
                     FloatRegister rm) {
  Emit(FloatBinary(0x1E203800u, width, rd, rn, rm));
}
void Assembler::FMul(FloatWidth width,
                     FloatRegister rd,
                     FloatRegister rn,
                     FloatRegister rm) {
  Emit(FloatBinary(0x1E200800u, width, rd, rn, rm));
}
void Assembler::FDiv(FloatWidth width,
                     FloatRegister rd,
                     FloatRegister rn,
                     FloatRegister rm) {
  Emit(FloatBinary(0x1E201800u, width, rd, rn, rm));
}
void Assembler::FNeg(FloatWidth width, FloatRegister rd, FloatRegister rn) {
  Emit(0x1E214000u | Ftype(width) | (Num(rn) << 5) | Num(rd));
}
void Assembler::FMov(FloatWidth width, FloatRegister rd, FloatRegister rn) {
  Emit(0x1E204000u | Ftype(width) | (Num(rn) << 5) | Num(rd));
}
void Assembler::FCmp(FloatWidth width, FloatRegister rn, FloatRegister rm) {
  Emit(0x1E202000u | Ftype(width) | (Num(rm) << 16) | (Num(rn) << 5));
}
void Assembler::FCvt(FloatWidth to,
                     FloatWidth from,
                     FloatRegister rd,
                     FloatRegister rn) {
  WOLANG_CHECK(to != from);
  const uint32_t base =
      to == FloatWidth::k64 ? 0x1E22C000u : 0x1E624000u;
  Emit(base | (Num(rn) << 5) | Num(rd));
}
void Assembler::Scvtf(FloatWidth width,
                      FloatRegister rd,
                      Width from,
                      Register rn) {
  Emit(0x1E220000u | Sf(from) | Ftype(width) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Ucvtf(FloatWidth width,
                      FloatRegister rd,
                      Width from,
                      Register rn) {
  Emit(0x1E230000u | Sf(from) | Ftype(width) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Fcvtzs(Width width,
                       Register rd,
                       FloatWidth from,
                       FloatRegister rn) {
  Emit(0x1E380000u | Sf(width) | Ftype(from) | (Num(rn) << 5) | Num(rd));
}
void Assembler::Fcvtzu(Width width,
                       Register rd,
                       FloatWidth from,
                       FloatRegister rn) {
  Emit(0x1E390000u | Sf(width) | Ftype(from) | (Num(rn) << 5) | Num(rd));
}
void Assembler::FMovToGeneral(Width width,
                              Register rd,
                              FloatRegister rn) {
  const uint32_t base =
      width == Width::k64 ? 0x9E660000u : 0x1E260000u;
  Emit(base | (Num(rn) << 5) | Num(rd));
}
void Assembler::FMovFromGeneral(FloatWidth width,
                                FloatRegister rd,
                                Register rn) {
  const uint32_t base =
      width == FloatWidth::k64 ? 0x9E670000u : 0x1E270000u;
  Emit(base | (Num(rn) << 5) | Num(rd));
}

}  // namespace arm64
}  // namespace codegen
}  // namespace wolang
