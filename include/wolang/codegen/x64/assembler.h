// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_X64_ASSEMBLER_H_
#define WOLANG_CODEGEN_X64_ASSEMBLER_H_

#include <cstdint>
#include <vector>

namespace wolang {
namespace codegen {
namespace x64 {

// An x86-64 encoder.
//
// Where the arm64 one copies a word and patches bit fields, this one builds
// bytes: a prefix if the operands need one, an opcode, a ModRM byte that
// says which registers or which memory, and whatever displacement follows.
// The shape is different and the discipline is the same -- every encoding
// here was compared against a system assembler's, because an encoder that
// is almost right produces code that almost works.
//
// Only what the dev tier emits is here.

enum class Register : uint8_t {
  kRax = 0, kRcx, kRdx, kRbx, kRsp, kRbp, kRsi, kRdi,
  kR8, kR9, kR10, kR11, kR12, kR13, kR14, kR15,
};

enum class XmmRegister : uint8_t {
  kXmm0 = 0, kXmm1, kXmm2, kXmm3, kXmm4, kXmm5, kXmm6, kXmm7,
  kXmm8, kXmm9, kXmm10, kXmm11, kXmm12, kXmm13, kXmm14, kXmm15,
};

// The operand size of an integer instruction.
enum class Width : uint8_t { k8, k16, k32, k64 };
// `ss` or `sd`: a single or a double.
enum class FloatWidth : uint8_t { k32, k64 };

// Condition codes, in their encoded order; the ones the dev tier uses have
// names here and the rest are left out rather than guessed at.
enum class Condition : uint8_t {
  kOverflow = 0x0,
  kNoOverflow = 0x1,
  kBelow = 0x2,          // unsigned <
  kAboveEqual = 0x3,     // unsigned >=
  kEqual = 0x4,
  kNotEqual = 0x5,
  kBelowEqual = 0x6,     // unsigned <=
  kAbove = 0x7,          // unsigned >
  kSign = 0x8,
  kNoSign = 0x9,
  kParity = 0xA,
  kNoParity = 0xB,
  kLess = 0xC,           // signed <
  kGreaterEqual = 0xD,   // signed >=
  kLessEqual = 0xE,      // signed <=
  kGreater = 0xF,        // signed >
};

Condition Invert(Condition condition);

struct Label {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
};

// A memory operand: `[base + displacement]`, which is every address this
// tier needs -- a frame slot, a field, or an element whose index has
// already been multiplied out.
struct Address {
  Register base = Register::kRsp;
  int32_t displacement = 0;
};

class Assembler {
 public:
  Assembler() = default;

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  uint32_t offset() const { return static_cast<uint32_t>(bytes_.size()); }

  // Patches every jump to a bound label. A jump to a label nothing bound is
  // a bug in the emitter, not in the program being compiled.
  void Resolve();

  Label CreateLabel();
  void Bind(Label label);
  uint32_t OffsetOf(Label label) const;

  // ------------------------------------------------------------- moves ---

  void Mov(Width width, Register destination, Register source);
  void MovImmediate(Width width, Register destination, uint64_t value);
  void Load(Width width, Register destination, Address address);
  void Store(Width width, Address address, Register source);
  // Sign and zero extension, which is how a narrow integer keeps the value
  // its width says it has.
  void Movsx(Width from, Register destination, Register source);
  void Movzx(Width from, Register destination, Register source);
  void LoadSigned(Width width, Register destination, Address address);
  void LoadUnsigned(Width width, Register destination, Address address);
  // `lea`, which is how an address becomes a value.
  void Lea(Register destination, Address address);

  // -------------------------------------------------------- arithmetic ---

  void Add(Width width, Register destination, Register source);
  void Sub(Width width, Register destination, Register source);
  void Imul(Width width, Register destination, Register source);
  void AddImmediate(Width width, Register destination, int32_t value);
  void SubImmediate(Width width, Register destination, int32_t value);
  void Neg(Width width, Register destination);
  void Not(Width width, Register destination);
  void And(Width width, Register destination, Register source);
  void Or(Width width, Register destination, Register source);
  void Xor(Width width, Register destination, Register source);
  // Shifts by `cl`, which is the only variable shift x86 has.
  void ShlCl(Width width, Register destination);
  void ShrCl(Width width, Register destination);
  void SarCl(Width width, Register destination);
  void ShrImmediate(Width width, Register destination, uint8_t amount);
  void SarImmediate(Width width, Register destination, uint8_t amount);
  // Sign extends `rax` into `rdx`, which `idiv` needs.
  void Cqo(Width width);
  void Idiv(Width width, Register divisor);
  void Div(Width width, Register divisor);

  void Cmp(Width width, Register left, Register right);
  void CmpImmediate(Width width, Register left, int32_t value);
  void Test(Width width, Register left, Register right);
  void Setcc(Condition condition, Register destination);

  // --------------------------------------------------------- branches ---

  void Jmp(Label label);
  void Jcc(Condition condition, Label label);
  void CallRelative(Label label);
  // A call whose target is not known yet: five bytes, patched later.
  void CallPlaceholder();
  void CallRegister(Register target);
  void Ret();
  void Push(Register source);
  void Pop(Register destination);
  void Int3();
  void Nop();

  // --------------------------------------------------- floating point ---

  void MovFloat(FloatWidth width, XmmRegister destination,
                XmmRegister source);
  void LoadFloat(FloatWidth width, XmmRegister destination, Address address);
  void StoreFloat(FloatWidth width, Address address, XmmRegister source);
  void AddFloat(FloatWidth width, XmmRegister destination,
                XmmRegister source);
  void SubFloat(FloatWidth width, XmmRegister destination,
                XmmRegister source);
  void MulFloat(FloatWidth width, XmmRegister destination,
                XmmRegister source);
  void DivFloat(FloatWidth width, XmmRegister destination,
                XmmRegister source);
  void XorFloat(XmmRegister destination, XmmRegister source);
  // An ordered compare that sets the integer flags.
  void CompareFloat(FloatWidth width, XmmRegister left, XmmRegister right);
  void ConvertFloatWidth(FloatWidth to, XmmRegister destination,
                         XmmRegister source);
  void IntToFloat(FloatWidth width, XmmRegister destination, Width from,
                  Register source);
  void FloatToInt(Width width, Register destination, FloatWidth from,
                  XmmRegister source);
  // The bits, not the value.
  void MovFloatToGeneral(Width width, Register destination,
                         XmmRegister source);
  void MovGeneralToFloat(Width width, XmmRegister destination,
                         Register source);

 private:
  struct Fixup {
    // Where the four-byte displacement starts.
    uint32_t at = 0;
    Label label;
  };

  void Emit(uint8_t byte) { bytes_.push_back(byte); }
  void Emit32(uint32_t value);
  void Emit64(uint64_t value);
  // The REX prefix, when the width or a high register needs one.
  void Rex(Width width, uint8_t reg, uint8_t rm, bool force = false);
  void ModRegRm(uint8_t reg, uint8_t rm);
  void ModRegAddress(uint8_t reg, Address address);
  void Prefix(Width width);

  std::vector<uint8_t> bytes_;
  std::vector<uint32_t> labels_;
  std::vector<Fixup> fixups_;
};

}  // namespace x64
}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_X64_ASSEMBLER_H_
