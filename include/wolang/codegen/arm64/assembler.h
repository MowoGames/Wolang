// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_ARM64_ASSEMBLER_H_
#define WOLANG_CODEGEN_ARM64_ASSEMBLER_H_

#include <cstdint>
#include <vector>

namespace wolang {
namespace codegen {
namespace arm64 {

// A64 instructions are all four bytes wide, which is what makes
// copy-and-patch natural on this target: a stencil is a handful of words and
// patching an operand is writing a bit field. This assembler is the patch
// step -- every method below is "copy this word, with these fields filled
// in".
//
// Only what the dev tier emits is here. A64 is a large encoding and the
// instructions nothing generates would be untested bytes.

enum class Register : uint8_t {
  kX0 = 0, kX1, kX2, kX3, kX4, kX5, kX6, kX7,
  kX8, kX9, kX10, kX11, kX12, kX13, kX14, kX15,
  kX16, kX17, kX18, kX19, kX20, kX21, kX22, kX23,
  kX24, kX25, kX26, kX27, kX28, kX29, kX30,
  // Register 31 is the zero register in most instructions and the stack
  // pointer in a few. Which one it is depends on the opcode, not on the
  // operand, so both names are the same number and the reader has to know
  // the instruction -- exactly as the architecture has it.
  kZr = 31,
  kSp = 31,
};

enum class FloatRegister : uint8_t {
  kV0 = 0, kV1, kV2, kV3, kV4, kV5, kV6, kV7,
  kV8, kV9, kV10, kV11, kV12, kV13, kV14, kV15,
  kV16, kV17, kV18, kV19, kV20, kV21, kV22, kV23,
  kV24, kV25, kV26, kV27, kV28, kV29, kV30, kV31,
};

// The operand size of an integer instruction: `w` registers or `x` ones.
enum class Width : uint8_t { k32, k64 };
// The operand size of a floating point instruction: `s` or `d`.
enum class FloatWidth : uint8_t { k32, k64 };

// Condition codes, in their encoded order.
enum class Condition : uint8_t {
  kEq = 0, kNe, kHs, kLo, kMi, kPl, kVs, kVc,
  kHi, kLs, kGe, kLt, kGt, kLe, kAl,
};

// The condition that is true exactly when |condition| is not.
Condition Invert(Condition condition);

// A place in the instruction stream a branch can name before it is known.
struct Label {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
};

class Assembler {
 public:
  Assembler() = default;

  // ----------------------------------------------------------- the buffer --

  const std::vector<uint32_t>& words() const { return words_; }
  // Where the next instruction lands, in bytes from the start.
  uint32_t offset() const {
    return static_cast<uint32_t>(words_.size() * sizeof(uint32_t));
  }
  // Patches every branch to a bound label. A branch to a label that was
  // never bound is a bug in the emitter, not in the program being compiled,
  // so it checks rather than reporting.
  void Resolve();

  // ------------------------------------------------------------- labels ---

  Label CreateLabel();
  void Bind(Label label);
  // Where |label| ended up, for a jump table or a relocation.
  uint32_t OffsetOf(Label label) const;

  // -------------------------------------------------------------- moves ---

  void Movz(Width width, Register rd, uint16_t value, unsigned shift = 0);
  void Movk(Width width, Register rd, uint16_t value, unsigned shift = 0);
  void Movn(Width width, Register rd, uint16_t value, unsigned shift = 0);
  // `mov rd, rn`, which is `orr rd, zr, rn`.
  void Mov(Width width, Register rd, Register rn);
  // The shortest `movz`/`movk`/`movn` sequence that lands |value| in |rd|.
  // Up to four words, which is the cost of an arbitrary 64-bit constant on
  // a fixed width encoding.
  void MovImmediate(Width width, Register rd, uint64_t value);

  // --------------------------------------------------------- arithmetic ---

  void Add(Width width, Register rd, Register rn, Register rm);
  void Sub(Width width, Register rd, Register rn, Register rm);
  // The flag-setting forms, which is how overflow is detected.
  void Adds(Width width, Register rd, Register rn, Register rm);
  void Subs(Width width, Register rd, Register rn, Register rm);
  void AddImm(Width width, Register rd, Register rn, uint32_t value);
  void SubImm(Width width, Register rd, Register rn, uint32_t value);
  void SubsImm(Width width, Register rd, Register rn, uint32_t value);

  void Mul(Width width, Register rd, Register rn, Register rm);
  // `rd = ra - rn * rm`, which is how a remainder is computed.
  void Msub(Width width, Register rd, Register rn, Register rm, Register ra);
  void Sdiv(Width width, Register rd, Register rn, Register rm);
  void Udiv(Width width, Register rd, Register rn, Register rm);
  // The high half of a multiplication, which is what tells a 64-bit
  // multiply whether it overflowed.
  void Smulh(Register rd, Register rn, Register rm);
  void Umulh(Register rd, Register rn, Register rm);

  void And(Width width, Register rd, Register rn, Register rm);
  void Orr(Width width, Register rd, Register rn, Register rm);
  void Eor(Width width, Register rd, Register rn, Register rm);
  // `mvn rd, rn`, which is `orn rd, zr, rn`.
  void Mvn(Width width, Register rd, Register rn);

  void Lslv(Width width, Register rd, Register rn, Register rm);
  void Lsrv(Width width, Register rd, Register rn, Register rm);
  void Asrv(Width width, Register rd, Register rn, Register rm);
  void Lsl(Width width, Register rd, Register rn, unsigned amount);
  void Lsr(Width width, Register rd, Register rn, unsigned amount);
  void Asr(Width width, Register rd, Register rn, unsigned amount);

  // Sign and zero extension, which is what keeps a narrow integer's value
  // the one its width says it is.
  void Sxtb(Width width, Register rd, Register rn);
  void Sxth(Width width, Register rd, Register rn);
  void Sxtw(Register rd, Register rn);
  void Uxtb(Register rd, Register rn);
  void Uxth(Register rd, Register rn);
  // Zero extends a 32-bit value into 64 bits, which a `mov w` already does.
  void Uxtw(Register rd, Register rn);

  // ------------------------------------------------------- comparisons ---

  void Cmp(Width width, Register rn, Register rm);
  void CmpImm(Width width, Register rn, uint32_t value);
  void Cset(Width width, Register rd, Condition condition);
  void Csel(Width width,
            Register rd,
            Register rn,
            Register rm,
            Condition condition);

  // ----------------------------------------------------------- memory ---

  // |offset| is in bytes and may be negative; the scaled form is used where
  // it fits and the unscaled one otherwise.
  void Ldr(Width width, Register rt, Register base, int32_t offset);
  void Str(Width width, Register rt, Register base, int32_t offset);
  void Ldrb(Register rt, Register base, int32_t offset);
  void Ldrsb(Width width, Register rt, Register base, int32_t offset);
  void Strb(Register rt, Register base, int32_t offset);
  void Ldrh(Register rt, Register base, int32_t offset);
  void Ldrsh(Width width, Register rt, Register base, int32_t offset);
  void Strh(Register rt, Register base, int32_t offset);
  void Ldrsw(Register rt, Register base, int32_t offset);

  void LdrFloat(FloatWidth width,
                FloatRegister rt,
                Register base,
                int32_t offset);
  void StrFloat(FloatWidth width,
                FloatRegister rt,
                Register base,
                int32_t offset);

  // The prologue and epilogue pair: `stp x29, x30, [sp, #offset]!` and
  // `ldp x29, x30, [sp], #offset`.
  void StpPreIndex(Register rt1, Register rt2, Register base, int32_t offset);
  void LdpPostIndex(Register rt1, Register rt2, Register base, int32_t offset);
  void Stp(Register rt1, Register rt2, Register base, int32_t offset);
  void Ldp(Register rt1, Register rt2, Register base, int32_t offset);

  // --------------------------------------------------------- branches ---

  void B(Label label);
  void Bl(Label label);
  // A call whose target is not in this buffer. The word is emitted with a
  // zero displacement for a later pass to patch, which is what linking a
  // module's functions together comes down to.
  void BlPlaceholder();
  void BCond(Condition condition, Label label);
  void Cbz(Width width, Register rn, Label label);
  void Cbnz(Width width, Register rn, Label label);
  void Br(Register rn);
  void Blr(Register rn);
  void Ret();
  void Brk(uint16_t reason);
  void Nop();

  // --------------------------------------------------- floating point ---

  void FAdd(FloatWidth width,
            FloatRegister rd,
            FloatRegister rn,
            FloatRegister rm);
  void FSub(FloatWidth width,
            FloatRegister rd,
            FloatRegister rn,
            FloatRegister rm);
  void FMul(FloatWidth width,
            FloatRegister rd,
            FloatRegister rn,
            FloatRegister rm);
  void FDiv(FloatWidth width,
            FloatRegister rd,
            FloatRegister rn,
            FloatRegister rm);
  void FNeg(FloatWidth width, FloatRegister rd, FloatRegister rn);
  void FMov(FloatWidth width, FloatRegister rd, FloatRegister rn);
  void FCmp(FloatWidth width, FloatRegister rn, FloatRegister rm);
  // Between the two float widths.
  void FCvt(FloatWidth to, FloatWidth from, FloatRegister rd,
            FloatRegister rn);
  // Integer to float and back. The float to integer direction truncates
  // toward zero, which is what `as` means.
  void Scvtf(FloatWidth width, FloatRegister rd, Width from, Register rn);
  void Ucvtf(FloatWidth width, FloatRegister rd, Width from, Register rn);
  void Fcvtzs(Width width, Register rd, FloatWidth from, FloatRegister rn);
  void Fcvtzu(Width width, Register rd, FloatWidth from, FloatRegister rn);
  // The bits, not the value: `fmov x0, d0`.
  void FMovToGeneral(Width width, Register rd, FloatRegister rn);
  void FMovFromGeneral(FloatWidth width, FloatRegister rd, Register rn);

 private:
  enum class FixupKind : uint8_t {
    // The 26-bit offset of an unconditional branch.
    kBranch26,
    // The 19-bit offset of a conditional branch or a compare-and-branch.
    kBranch19,
  };

  struct Fixup {
    uint32_t word = 0;
    Label label;
    FixupKind kind = FixupKind::kBranch26;
  };

  void Emit(uint32_t word) { words_.push_back(word); }
  void EmitBranch(uint32_t word, Label label, FixupKind kind);
  void LoadStore(uint32_t scaled,
                 uint32_t unscaled,
                 unsigned size_log2,
                 uint32_t rt,
                 Register base,
                 int32_t offset);

  std::vector<uint32_t> words_;
  // Where each label landed, or an unbound marker.
  std::vector<uint32_t> labels_;
  std::vector<Fixup> fixups_;
};

}  // namespace arm64
}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_ARM64_ASSEMBLER_H_
