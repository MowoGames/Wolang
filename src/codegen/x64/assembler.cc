// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/x64/assembler.h"

#include "wolang/base/check.h"

namespace wolang {
namespace codegen {
namespace x64 {
namespace {

constexpr uint32_t kUnbound = 0xFFFFFFFFu;

uint8_t Num(Register reg) {
  return static_cast<uint8_t>(reg) & 0x0F;
}
uint8_t Num(XmmRegister reg) {
  return static_cast<uint8_t>(reg) & 0x0F;
}

}  // namespace

Condition Invert(Condition condition) {
  // The architecture pairs conditions so that flipping the low bit inverts
  // the test, the same way arm64 does.
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
  labels_[label.index] = static_cast<uint32_t>(bytes_.size());
}

uint32_t Assembler::OffsetOf(Label label) const {
  WOLANG_CHECK(label.is_valid() && label.index < labels_.size());
  WOLANG_CHECK(labels_[label.index] != kUnbound);
  return labels_[label.index];
}

void Assembler::Resolve() {
  for (const Fixup& fixup : fixups_) {
    WOLANG_CHECK(fixup.label.index < labels_.size());
    const uint32_t target = labels_[fixup.label.index];
    WOLANG_CHECK(target != kUnbound);
    // A relative displacement counts from the end of the instruction, which
    // is the four bytes being patched.
    const int32_t distance = static_cast<int32_t>(target) -
                             static_cast<int32_t>(fixup.at + 4);
    const auto raw = static_cast<uint32_t>(distance);
    bytes_[fixup.at] = static_cast<uint8_t>(raw & 0xFF);
    bytes_[fixup.at + 1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
    bytes_[fixup.at + 2] = static_cast<uint8_t>((raw >> 16) & 0xFF);
    bytes_[fixup.at + 3] = static_cast<uint8_t>((raw >> 24) & 0xFF);
  }
  fixups_.clear();
}

// ---------------------------------------------------------- the encoding ---

void Assembler::Emit32(uint32_t value) {
  Emit(static_cast<uint8_t>(value & 0xFF));
  Emit(static_cast<uint8_t>((value >> 8) & 0xFF));
  Emit(static_cast<uint8_t>((value >> 16) & 0xFF));
  Emit(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void Assembler::Emit64(uint64_t value) {
  Emit32(static_cast<uint32_t>(value & 0xFFFFFFFFu));
  Emit32(static_cast<uint32_t>(value >> 32));
}

void Assembler::Prefix(Width width) {
  if (width == Width::k16) {
    Emit(0x66);
  }
}

// REX carries the fourth bit of each register number and the "this is 64
// bits" flag. It is also what makes `sil`, `dil` and friends addressable,
// which is why a byte operation on one of those forces it.
void Assembler::Rex(Width width, uint8_t reg, uint8_t rm, bool force) {
  uint8_t rex = 0;
  if (width == Width::k64) {
    rex |= 0x08;
  }
  if ((reg & 0x08) != 0) {
    rex |= 0x04;
  }
  if ((rm & 0x08) != 0) {
    rex |= 0x01;
  }
  if (rex != 0 || force) {
    Emit(static_cast<uint8_t>(0x40 | rex));
  }
}

void Assembler::ModRegRm(uint8_t reg, uint8_t rm) {
  Emit(static_cast<uint8_t>(0xC0 | ((reg & 0x07) << 3) | (rm & 0x07)));
}

// `[base + displacement]`, with the short forms the common cases deserve.
// `rsp` and `r12` need a SIB byte because their low three bits mean "a SIB
// follows" rather than "this register"; `rbp` and `r13` need an explicit
// zero displacement for the same kind of reason.
void Assembler::ModRegAddress(uint8_t reg, Address address) {
  const uint8_t base = Num(address.base) & 0x07;
  const int32_t displacement = address.displacement;
  const bool needs_sib = base == 0x04;
  const bool needs_displacement = displacement != 0 || base == 0x05;

  uint8_t mode = 0x00;
  if (needs_displacement) {
    mode = displacement >= -128 && displacement <= 127 ? 0x40 : 0x80;
  }
  Emit(static_cast<uint8_t>(mode | ((reg & 0x07) << 3) |
                            (needs_sib ? 0x04 : base)));
  if (needs_sib) {
    // scale 1, no index, this base.
    Emit(static_cast<uint8_t>(0x20 | base));
  }
  if (mode == 0x40) {
    Emit(static_cast<uint8_t>(static_cast<int8_t>(displacement)));
  } else if (mode == 0x80) {
    Emit32(static_cast<uint32_t>(displacement));
  }
}

// ----------------------------------------------------------------- moves ---

void Assembler::Mov(Width width, Register destination, Register source) {
  Prefix(width);
  Rex(width, Num(source), Num(destination));
  Emit(width == Width::k8 ? 0x88 : 0x89);
  ModRegRm(Num(source), Num(destination));
}

void Assembler::MovImmediate(Width width, Register destination,
                             uint64_t value) {
  if (width == Width::k64 &&
      value > 0xFFFFFFFFull) {
    // Ten bytes, and the only way to say a whole 64-bit constant.
    Rex(Width::k64, 0, Num(destination));
    Emit(static_cast<uint8_t>(0xB8 | (Num(destination) & 0x07)));
    Emit64(value);
    return;
  }
  // Writing a 32-bit register zeroes the top half, which is what makes this
  // the right encoding for every value that fits.
  Rex(Width::k32, 0, Num(destination));
  Emit(static_cast<uint8_t>(0xB8 | (Num(destination) & 0x07)));
  Emit32(static_cast<uint32_t>(value & 0xFFFFFFFFull));
}

void Assembler::Load(Width width, Register destination, Address address) {
  Prefix(width);
  Rex(width, Num(destination), Num(address.base));
  Emit(width == Width::k8 ? 0x8A : 0x8B);
  ModRegAddress(Num(destination), address);
}

void Assembler::Store(Width width, Address address, Register source) {
  Prefix(width);
  Rex(width, Num(source), Num(address.base),
      width == Width::k8 && Num(source) >= 4);
  Emit(width == Width::k8 ? 0x88 : 0x89);
  ModRegAddress(Num(source), address);
}

void Assembler::Movsx(Width from, Register destination, Register source) {
  if (from == Width::k32) {
    Rex(Width::k64, Num(destination), Num(source));
    Emit(0x63);
    ModRegRm(Num(destination), Num(source));
    return;
  }
  Rex(Width::k64, Num(destination), Num(source),
      from == Width::k8 && Num(source) >= 4);
  Emit(0x0F);
  Emit(from == Width::k8 ? 0xBE : 0xBF);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::Movzx(Width from, Register destination, Register source) {
  WOLANG_CHECK(from == Width::k8 || from == Width::k16);
  // Zero extending into 32 bits also clears the top half, so the 32-bit
  // form is enough and is a byte shorter.
  Rex(Width::k32, Num(destination), Num(source),
      from == Width::k8 && Num(source) >= 4);
  Emit(0x0F);
  Emit(from == Width::k8 ? 0xB6 : 0xB7);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::LoadSigned(Width width, Register destination,
                           Address address) {
  if (width == Width::k32) {
    Rex(Width::k64, Num(destination), Num(address.base));
    Emit(0x63);
    ModRegAddress(Num(destination), address);
    return;
  }
  Rex(Width::k64, Num(destination), Num(address.base));
  Emit(0x0F);
  Emit(width == Width::k8 ? 0xBE : 0xBF);
  ModRegAddress(Num(destination), address);
}

void Assembler::LoadUnsigned(Width width, Register destination,
                             Address address) {
  WOLANG_CHECK(width == Width::k8 || width == Width::k16);
  Rex(Width::k32, Num(destination), Num(address.base));
  Emit(0x0F);
  Emit(width == Width::k8 ? 0xB6 : 0xB7);
  ModRegAddress(Num(destination), address);
}

void Assembler::Lea(Register destination, Address address) {
  Rex(Width::k64, Num(destination), Num(address.base));
  Emit(0x8D);
  ModRegAddress(Num(destination), address);
}

// ------------------------------------------------------------ arithmetic ---

namespace {

// The two-operand integer instructions differ only in one opcode byte.
struct Binary {
  uint8_t opcode;
};

}  // namespace

void Assembler::Add(Width width, Register destination, Register source) {
  Prefix(width);
  Rex(width, Num(source), Num(destination));
  Emit(width == Width::k8 ? 0x00 : 0x01);
  ModRegRm(Num(source), Num(destination));
}

void Assembler::Sub(Width width, Register destination, Register source) {
  Prefix(width);
  Rex(width, Num(source), Num(destination));
  Emit(width == Width::k8 ? 0x28 : 0x29);
  ModRegRm(Num(source), Num(destination));
}

void Assembler::Imul(Width width, Register destination, Register source) {
  Prefix(width);
  Rex(width, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0xAF);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::AddImmediate(Width width, Register destination,
                             int32_t value) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  if (value >= -128 && value <= 127) {
    Emit(0x83);
    ModRegRm(0, Num(destination));
    Emit(static_cast<uint8_t>(static_cast<int8_t>(value)));
    return;
  }
  Emit(0x81);
  ModRegRm(0, Num(destination));
  Emit32(static_cast<uint32_t>(value));
}

void Assembler::SubImmediate(Width width, Register destination,
                             int32_t value) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  if (value >= -128 && value <= 127) {
    Emit(0x83);
    ModRegRm(5, Num(destination));
    Emit(static_cast<uint8_t>(static_cast<int8_t>(value)));
    return;
  }
  Emit(0x81);
  ModRegRm(5, Num(destination));
  Emit32(static_cast<uint32_t>(value));
}

void Assembler::Neg(Width width, Register destination) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  Emit(width == Width::k8 ? 0xF6 : 0xF7);
  ModRegRm(3, Num(destination));
}

void Assembler::Not(Width width, Register destination) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  Emit(width == Width::k8 ? 0xF6 : 0xF7);
  ModRegRm(2, Num(destination));
}

void Assembler::And(Width width, Register destination, Register source) {
  Prefix(width);
  Rex(width, Num(source), Num(destination));
  Emit(width == Width::k8 ? 0x20 : 0x21);
  ModRegRm(Num(source), Num(destination));
}

void Assembler::Or(Width width, Register destination, Register source) {
  Prefix(width);
  Rex(width, Num(source), Num(destination));
  Emit(width == Width::k8 ? 0x08 : 0x09);
  ModRegRm(Num(source), Num(destination));
}

void Assembler::Xor(Width width, Register destination, Register source) {
  Prefix(width);
  Rex(width, Num(source), Num(destination));
  Emit(width == Width::k8 ? 0x30 : 0x31);
  ModRegRm(Num(source), Num(destination));
}

void Assembler::ShlCl(Width width, Register destination) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  Emit(0xD3);
  ModRegRm(4, Num(destination));
}

void Assembler::ShrCl(Width width, Register destination) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  Emit(0xD3);
  ModRegRm(5, Num(destination));
}

void Assembler::SarCl(Width width, Register destination) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  Emit(0xD3);
  ModRegRm(7, Num(destination));
}

void Assembler::ShrImmediate(Width width, Register destination,
                             uint8_t amount) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  Emit(0xC1);
  ModRegRm(5, Num(destination));
  Emit(amount);
}

void Assembler::SarImmediate(Width width, Register destination,
                             uint8_t amount) {
  Prefix(width);
  Rex(width, 0, Num(destination));
  Emit(0xC1);
  ModRegRm(7, Num(destination));
  Emit(amount);
}

void Assembler::Cqo(Width width) {
  Prefix(width);
  Rex(width, 0, 0);
  Emit(0x99);
}

void Assembler::Idiv(Width width, Register divisor) {
  Prefix(width);
  Rex(width, 0, Num(divisor));
  Emit(0xF7);
  ModRegRm(7, Num(divisor));
}

void Assembler::Div(Width width, Register divisor) {
  Prefix(width);
  Rex(width, 0, Num(divisor));
  Emit(0xF7);
  ModRegRm(6, Num(divisor));
}

void Assembler::Cmp(Width width, Register left, Register right) {
  Prefix(width);
  Rex(width, Num(right), Num(left));
  Emit(width == Width::k8 ? 0x38 : 0x39);
  ModRegRm(Num(right), Num(left));
}

void Assembler::CmpImmediate(Width width, Register left, int32_t value) {
  Prefix(width);
  Rex(width, 0, Num(left));
  if (value >= -128 && value <= 127) {
    Emit(0x83);
    ModRegRm(7, Num(left));
    Emit(static_cast<uint8_t>(static_cast<int8_t>(value)));
    return;
  }
  Emit(0x81);
  ModRegRm(7, Num(left));
  Emit32(static_cast<uint32_t>(value));
}

void Assembler::Test(Width width, Register left, Register right) {
  Prefix(width);
  Rex(width, Num(right), Num(left));
  Emit(width == Width::k8 ? 0x84 : 0x85);
  ModRegRm(Num(right), Num(left));
}

void Assembler::Setcc(Condition condition, Register destination) {
  // The byte registers above `rbx` need a REX prefix to be nameable at all.
  Rex(Width::k8, 0, Num(destination), Num(destination) >= 4);
  Emit(0x0F);
  Emit(static_cast<uint8_t>(0x90 | static_cast<uint8_t>(condition)));
  ModRegRm(0, Num(destination));
}

// -------------------------------------------------------------- branches ---

void Assembler::Jmp(Label label) {
  Emit(0xE9);
  fixups_.push_back(Fixup{static_cast<uint32_t>(bytes_.size()), label});
  Emit32(0);
}

void Assembler::Jcc(Condition condition, Label label) {
  Emit(0x0F);
  Emit(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(condition)));
  fixups_.push_back(Fixup{static_cast<uint32_t>(bytes_.size()), label});
  Emit32(0);
}

void Assembler::CallRelative(Label label) {
  Emit(0xE8);
  fixups_.push_back(Fixup{static_cast<uint32_t>(bytes_.size()), label});
  Emit32(0);
}

void Assembler::CallPlaceholder() {
  Emit(0xE8);
  Emit32(0);
}

void Assembler::CallRegister(Register target) {
  Rex(Width::k32, 0, Num(target));
  Emit(0xFF);
  ModRegRm(2, Num(target));
}

void Assembler::Ret() {
  Emit(0xC3);
}

void Assembler::Push(Register source) {
  Rex(Width::k32, 0, Num(source));
  Emit(static_cast<uint8_t>(0x50 | (Num(source) & 0x07)));
}

void Assembler::Pop(Register destination) {
  Rex(Width::k32, 0, Num(destination));
  Emit(static_cast<uint8_t>(0x58 | (Num(destination) & 0x07)));
}

void Assembler::Int3() {
  Emit(0xCC);
}

void Assembler::Nop() {
  Emit(0x90);
}

// --------------------------------------------------------- floating point ---

namespace {

// `ss` instructions carry an F3 prefix and `sd` ones an F2; everything else
// about them is the same.
uint8_t ScalarPrefix(FloatWidth width) {
  return width == FloatWidth::k32 ? 0xF3 : 0xF2;
}

}  // namespace

void Assembler::MovFloat(FloatWidth width, XmmRegister destination,
                         XmmRegister source) {
  Emit(ScalarPrefix(width));
  Rex(Width::k32, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x10);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::LoadFloat(FloatWidth width, XmmRegister destination,
                          Address address) {
  Emit(ScalarPrefix(width));
  Rex(Width::k32, Num(destination), Num(address.base));
  Emit(0x0F);
  Emit(0x10);
  ModRegAddress(Num(destination), address);
}

void Assembler::StoreFloat(FloatWidth width, Address address,
                           XmmRegister source) {
  Emit(ScalarPrefix(width));
  Rex(Width::k32, Num(source), Num(address.base));
  Emit(0x0F);
  Emit(0x11);
  ModRegAddress(Num(source), address);
}

void Assembler::AddFloat(FloatWidth width, XmmRegister destination,
                         XmmRegister source) {
  Emit(ScalarPrefix(width));
  Rex(Width::k32, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x58);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::SubFloat(FloatWidth width, XmmRegister destination,
                         XmmRegister source) {
  Emit(ScalarPrefix(width));
  Rex(Width::k32, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x5C);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::MulFloat(FloatWidth width, XmmRegister destination,
                         XmmRegister source) {
  Emit(ScalarPrefix(width));
  Rex(Width::k32, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x59);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::DivFloat(FloatWidth width, XmmRegister destination,
                         XmmRegister source) {
  Emit(ScalarPrefix(width));
  Rex(Width::k32, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x5E);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::XorFloat(XmmRegister destination, XmmRegister source) {
  Rex(Width::k32, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x57);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::CompareFloat(FloatWidth width, XmmRegister left,
                             XmmRegister right) {
  // `ucomiss` and `ucomisd`: an unordered compare, which sets the parity
  // flag when either side is not a number -- the thing every float
  // comparison has to account for.
  if (width == FloatWidth::k64) {
    Emit(0x66);
  }
  Rex(Width::k32, Num(left), Num(right));
  Emit(0x0F);
  Emit(0x2E);
  ModRegRm(Num(left), Num(right));
}

void Assembler::ConvertFloatWidth(FloatWidth to, XmmRegister destination,
                                  XmmRegister source) {
  // Converting *to* single is `cvtsd2ss`, which carries the double prefix.
  Emit(to == FloatWidth::k32 ? 0xF2 : 0xF3);
  Rex(Width::k32, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x5A);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::IntToFloat(FloatWidth width, XmmRegister destination,
                           Width from, Register source) {
  Emit(ScalarPrefix(width));
  Rex(from == Width::k64 ? Width::k64 : Width::k32, Num(destination),
      Num(source));
  Emit(0x0F);
  Emit(0x2A);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::FloatToInt(Width width, Register destination,
                           FloatWidth from, XmmRegister source) {
  Emit(ScalarPrefix(from));
  Rex(width == Width::k64 ? Width::k64 : Width::k32, Num(destination),
      Num(source));
  Emit(0x0F);
  Emit(0x2C);
  ModRegRm(Num(destination), Num(source));
}

void Assembler::MovFloatToGeneral(Width width, Register destination,
                                  XmmRegister source) {
  Emit(0x66);
  Rex(width, Num(source), Num(destination));
  Emit(0x0F);
  Emit(0x7E);
  ModRegRm(Num(source), Num(destination));
}

void Assembler::MovGeneralToFloat(Width width, XmmRegister destination,
                                  Register source) {
  Emit(0x66);
  Rex(width, Num(destination), Num(source));
  Emit(0x0F);
  Emit(0x6E);
  ModRegRm(Num(destination), Num(source));
}

}  // namespace x64
}  // namespace codegen
}  // namespace wolang
