// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/x64/assembler.h"

#include <string>

#include "harness/test.h"

namespace wolang {
namespace codegen {
namespace x64 {
namespace {

// Every expectation below was produced by a system assembler
// (`clang -c -arch x86_64` and `objdump -d`) and copied here, not written
// from memory. On arm64 three hand-written expectations turned out to be
// wrong, and an encoder that is almost right produces code that almost
// works -- which is the worst kind.
std::string Hex(const Assembler& assembler) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (uint8_t byte : assembler.bytes()) {
    if (!text.empty()) {
      text.push_back(' ');
    }
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 0x0F]);
  }
  return text;
}

WOLANG_TEST(X64Assembler, EncodesMoves) {
  Assembler a;
  a.Mov(Width::k64, Register::kR12, Register::kRbx);
  a.Mov(Width::k32, Register::kRcx, Register::kRax);
  a.Mov(Width::k8, Register::kRbx, Register::kRax);
  WOLANG_EXPECT_EQ(Hex(a), "49 89 dc 89 c1 88 c3");
}

WOLANG_TEST(X64Assembler, EncodesImmediates) {
  Assembler a;
  // Writing a 32-bit register zeroes the top half, so every value that
  // fits in 32 bits gets the short form; this is a different encoding
  // from the assembler's `movq $5, %rax` and the same value.
  a.MovImmediate(Width::k64, Register::kRax, 5);
  WOLANG_EXPECT_EQ(Hex(a), "b8 05 00 00 00");

  Assembler big;
  big.MovImmediate(Width::k64, Register::kR13, 0x10000000000ull);
  WOLANG_EXPECT_EQ(Hex(big), "49 bd 00 00 00 00 00 01 00 00");

  // A negative value does not fit in 32 bits unsigned, so it takes the
  // long form -- which is the whole point of the test: the short form
  // would zero-extend it.
  Assembler negative;
  negative.MovImmediate(Width::k64, Register::kRax,
                        static_cast<uint64_t>(-1));
  WOLANG_EXPECT_EQ(Hex(negative), "48 b8 ff ff ff ff ff ff ff ff");
}

WOLANG_TEST(X64Assembler, EncodesMemoryOperands) {
  Assembler a;
  // `rsp` and `r12` need a SIB byte; `r13` needs an explicit zero
  // displacement. Three special cases that are easy to get wrong and
  // impossible to miss once they are wrong.
  a.Load(Width::k64, Register::kRax, Address{Register::kRsp, 0});
  a.Load(Width::k64, Register::kR12, Address{Register::kRsp, 16});
  a.Load(Width::k64, Register::kRbx, Address{Register::kR13, 1024});
  a.Load(Width::k32, Register::kRax, Address{Register::kR12, 0});
  a.Store(Width::k64, Address{Register::kRsp, 24}, Register::kR14);
  a.Store(Width::k8, Address{Register::kRbx, 7}, Register::kRbx);
  WOLANG_EXPECT_EQ(Hex(a),
                   "48 8b 04 24 "
                   "4c 8b 64 24 10 "
                   "49 8b 9d 00 04 00 00 "
                   "41 8b 04 24 "
                   "4c 89 74 24 18 "
                   "88 5b 07");
}

WOLANG_TEST(X64Assembler, EncodesExtensions) {
  Assembler a;
  a.Movsx(Width::k32, Register::kRbx, Register::kRax);
  a.Movsx(Width::k8, Register::kRax, Register::kRbx);
  a.Movsx(Width::k16, Register::kR13, Register::kR12);
  // `sil` is only addressable with a REX prefix, even an empty one.
  a.Movzx(Width::k8, Register::kRax, Register::kRsi);
  a.Movzx(Width::k16, Register::kR12, Register::kRbx);
  WOLANG_EXPECT_EQ(Hex(a),
                   "48 63 d8 "
                   "48 0f be c3 "
                   "4d 0f bf ec "
                   "40 0f b6 c6 "
                   "44 0f b7 e3");
}

WOLANG_TEST(X64Assembler, EncodesAddresses) {
  Assembler a;
  a.Lea(Register::kRax, Address{Register::kRsp, 32});
  a.Lea(Register::kRbx, Address{Register::kR12, 0});
  WOLANG_EXPECT_EQ(Hex(a), "48 8d 44 24 20 49 8d 1c 24");
}

WOLANG_TEST(X64Assembler, EncodesArithmetic) {
  Assembler a;
  a.Add(Width::k64, Register::kRax, Register::kRbx);
  a.Sub(Width::k32, Register::kRdx, Register::kRcx);
  a.Imul(Width::k64, Register::kRax, Register::kR12);
  a.AddImmediate(Width::k64, Register::kRsp, 8);
  a.SubImmediate(Width::k64, Register::kRsp, 4096);
  a.Neg(Width::k64, Register::kRax);
  a.Not(Width::k32, Register::kRax);
  WOLANG_EXPECT_EQ(Hex(a),
                   "48 01 d8 "
                   "29 ca "
                   "49 0f af c4 "
                   "48 83 c4 08 "
                   "48 81 ec 00 10 00 00 "
                   "48 f7 d8 "
                   "f7 d0");
}

WOLANG_TEST(X64Assembler, EncodesBitwiseAndShifts) {
  Assembler a;
  a.And(Width::k64, Register::kRax, Register::kRbx);
  a.Or(Width::k32, Register::kRax, Register::kRcx);
  a.Xor(Width::k64, Register::kRax, Register::kRax);
  a.ShlCl(Width::k64, Register::kRax);
  a.ShrCl(Width::k32, Register::kRax);
  a.SarCl(Width::k64, Register::kR12);
  a.ShrImmediate(Width::k64, Register::kRax, 3);
  a.SarImmediate(Width::k32, Register::kRax, 31);
  WOLANG_EXPECT_EQ(Hex(a),
                   "48 21 d8 "
                   "09 c8 "
                   "48 31 c0 "
                   "48 d3 e0 "
                   "d3 e8 "
                   "49 d3 fc "
                   "48 c1 e8 03 "
                   "c1 f8 1f");
}

WOLANG_TEST(X64Assembler, EncodesDivisionAndComparison) {
  Assembler a;
  a.Cqo(Width::k64);
  a.Cqo(Width::k32);
  a.Idiv(Width::k64, Register::kRbx);
  a.Div(Width::k32, Register::kRcx);
  a.Cmp(Width::k64, Register::kRax, Register::kRbx);
  a.CmpImmediate(Width::k32, Register::kRax, 100);
  a.Test(Width::k64, Register::kRax, Register::kRax);
  a.Setcc(Condition::kEqual, Register::kRax);
  a.Setcc(Condition::kLess, Register::kR12);
  a.Setcc(Condition::kBelow, Register::kRbx);
  WOLANG_EXPECT_EQ(Hex(a),
                   "48 99 "
                   "99 "
                   "48 f7 fb "
                   "f7 f1 "
                   "48 39 d8 "
                   "83 f8 64 "
                   "48 85 c0 "
                   "0f 94 c0 "
                   "41 0f 9c c4 "
                   "0f 92 c3");
}

WOLANG_TEST(X64Assembler, EncodesCallsAndStack) {
  Assembler a;
  a.Ret();
  a.Push(Register::kRbp);
  a.Push(Register::kR12);
  a.Pop(Register::kR15);
  a.CallRegister(Register::kRax);
  a.Int3();
  a.Nop();
  WOLANG_EXPECT_EQ(Hex(a), "c3 55 41 54 41 5f ff d0 cc 90");
}

WOLANG_TEST(X64Assembler, EncodesFloatMovesAndArithmetic) {
  Assembler a;
  a.MovFloat(FloatWidth::k32, XmmRegister::kXmm0, XmmRegister::kXmm1);
  a.MovFloat(FloatWidth::k64, XmmRegister::kXmm2, XmmRegister::kXmm9);
  a.LoadFloat(FloatWidth::k32, XmmRegister::kXmm0,
              Address{Register::kRsp, 0});
  a.LoadFloat(FloatWidth::k64, XmmRegister::kXmm11,
              Address{Register::kR12, 16});
  a.StoreFloat(FloatWidth::k32, Address{Register::kRsp, 8},
               XmmRegister::kXmm3);
  a.StoreFloat(FloatWidth::k64, Address{Register::kRbx, 0},
               XmmRegister::kXmm0);
  WOLANG_EXPECT_EQ(Hex(a),
                   "f3 0f 10 c1 "
                   "f2 41 0f 10 d1 "
                   "f3 0f 10 04 24 "
                   "f2 45 0f 10 5c 24 10 "
                   "f3 0f 11 5c 24 08 "
                   "f2 0f 11 03");

  Assembler b;
  b.AddFloat(FloatWidth::k32, XmmRegister::kXmm0, XmmRegister::kXmm1);
  b.AddFloat(FloatWidth::k64, XmmRegister::kXmm8, XmmRegister::kXmm9);
  b.SubFloat(FloatWidth::k32, XmmRegister::kXmm1, XmmRegister::kXmm2);
  b.MulFloat(FloatWidth::k64, XmmRegister::kXmm2, XmmRegister::kXmm3);
  b.DivFloat(FloatWidth::k32, XmmRegister::kXmm3, XmmRegister::kXmm4);
  b.XorFloat(XmmRegister::kXmm0, XmmRegister::kXmm1);
  WOLANG_EXPECT_EQ(Hex(b),
                   "f3 0f 58 c1 "
                   "f2 45 0f 58 c1 "
                   "f3 0f 5c ca "
                   "f2 0f 59 d3 "
                   "f3 0f 5e dc "
                   "0f 57 c1");
}

WOLANG_TEST(X64Assembler, EncodesFloatComparisonAndConversion) {
  Assembler a;
  a.CompareFloat(FloatWidth::k32, XmmRegister::kXmm0, XmmRegister::kXmm1);
  a.CompareFloat(FloatWidth::k64, XmmRegister::kXmm2, XmmRegister::kXmm3);
  a.ConvertFloatWidth(FloatWidth::k32, XmmRegister::kXmm0,
                      XmmRegister::kXmm1);
  a.ConvertFloatWidth(FloatWidth::k64, XmmRegister::kXmm0,
                      XmmRegister::kXmm1);
  a.IntToFloat(FloatWidth::k32, XmmRegister::kXmm0, Width::k32,
               Register::kRax);
  a.IntToFloat(FloatWidth::k64, XmmRegister::kXmm1, Width::k64,
               Register::kRbx);
  a.FloatToInt(Width::k32, Register::kRax, FloatWidth::k32,
               XmmRegister::kXmm0);
  a.FloatToInt(Width::k64, Register::kRbx, FloatWidth::k64,
               XmmRegister::kXmm1);
  WOLANG_EXPECT_EQ(Hex(a),
                   "0f 2e c1 "
                   "66 0f 2e d3 "
                   "f2 0f 5a c1 "
                   "f3 0f 5a c1 "
                   "f3 0f 2a c0 "
                   "f2 48 0f 2a cb "
                   "f3 0f 2c c0 "
                   "f2 48 0f 2c d9");
}

WOLANG_TEST(X64Assembler, EncodesBitwiseMovesBetweenBanks) {
  Assembler a;
  a.MovFloatToGeneral(Width::k64, Register::kRax, XmmRegister::kXmm0);
  a.MovFloatToGeneral(Width::k32, Register::kRcx, XmmRegister::kXmm1);
  a.MovGeneralToFloat(Width::k64, XmmRegister::kXmm0, Register::kRax);
  a.MovGeneralToFloat(Width::k32, XmmRegister::kXmm1, Register::kRcx);
  WOLANG_EXPECT_EQ(Hex(a),
                   "66 48 0f 7e c0 "
                   "66 0f 7e c9 "
                   "66 48 0f 6e c0 "
                   "66 0f 6e c9");
}

WOLANG_TEST(X64Assembler, ResolvesBranchesBothWays) {
  Assembler a;
  const Label forward = a.CreateLabel();
  const Label backward = a.CreateLabel();
  a.Bind(backward);
  a.Nop();
  a.Jmp(forward);
  a.Jcc(Condition::kEqual, backward);
  a.Bind(forward);
  a.Ret();
  a.Resolve();
  // The forward jump skips the six bytes of the conditional one; the
  // conditional jump goes back to offset zero, twelve bytes behind its
  // end, because a displacement counts from where the next instruction
  // starts and not from where this one does.
  WOLANG_EXPECT_EQ(Hex(a),
                   "90 "
                   "e9 06 00 00 00 "
                   "0f 84 f4 ff ff ff "
                   "c3");
  WOLANG_EXPECT_EQ(a.OffsetOf(forward), 12u);
}

WOLANG_TEST(X64Assembler, InvertsConditions) {
  WOLANG_EXPECT_EQ(Invert(Condition::kEqual), Condition::kNotEqual);
  WOLANG_EXPECT_EQ(Invert(Condition::kNotEqual), Condition::kEqual);
  WOLANG_EXPECT_EQ(Invert(Condition::kLess), Condition::kGreaterEqual);
  WOLANG_EXPECT_EQ(Invert(Condition::kAbove), Condition::kBelowEqual);
  WOLANG_EXPECT_EQ(Invert(Condition::kOverflow), Condition::kNoOverflow);
}

}  // namespace
}  // namespace x64
}  // namespace codegen
}  // namespace wolang
