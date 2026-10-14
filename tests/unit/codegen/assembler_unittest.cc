// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/arm64/assembler.h"

#include "harness/test.h"

namespace wolang {
namespace codegen {
namespace arm64 {
namespace {

using R = Register;
using F = FloatRegister;

constexpr Width k32 = Width::k32;
constexpr Width k64 = Width::k64;

// Every expectation below is a word a system assembler produced for the same
// instruction. An encoder that is almost right produces code that almost
// works, and nothing further up can tell the difference -- so the bytes are
// checked against an outside answer rather than against this file's own
// understanding of the manual.
WOLANG_TEST(AssemblerTest, EncodesWhatASystemAssemblerWould) {
  Assembler a;
  a.Movz(k64, R::kX0, 0x1234);
  a.Mov(k64, R::kX4, R::kX5);
  a.Add(k64, R::kX0, R::kX1, R::kX2);
  a.Add(k32, R::kX0, R::kX1, R::kX2);
  a.Subs(k32, R::kX9, R::kX10, R::kX11);
  a.AddImm(k64, R::kX0, R::kX1, 273);
  a.Mul(k64, R::kX0, R::kX1, R::kX2);
  a.Sdiv(k64, R::kX0, R::kX1, R::kX2);
  a.Smulh(R::kX0, R::kX1, R::kX2);
  a.Mvn(k64, R::kX9, R::kX10);
  a.Lsl(k64, R::kX0, R::kX1, 3);
  a.Sxtb(k64, R::kX0, R::kX1);
  a.Cset(k64, R::kX3, Condition::kEq);
  a.Ret();

  const std::vector<uint32_t> expected = {
      0xd2824680, 0xaa0503e4, 0x8b020020, 0x0b020020, 0x6b0b0149,
      0x91044420, 0x9b027c20, 0x9ac20c20, 0x9b427c20, 0xaa2a03e9,
      0xd37df020, 0x93401c20, 0x9a9f17e3, 0xd65f03c0,
  };
  WOLANG_EXPECT_EQ(a.words().size(), expected.size());
  for (size_t i = 0; i < expected.size() && i < a.words().size(); ++i) {
    WOLANG_EXPECT_EQ(a.words()[i], expected[i]);
  }
}

WOLANG_TEST(AssemblerTest, EncodesFloatingPoint) {
  Assembler a;
  a.FAdd(FloatWidth::k32, F::kV0, F::kV1, F::kV2);
  a.FAdd(FloatWidth::k64, F::kV3, F::kV4, F::kV5);
  a.FCmp(FloatWidth::k64, F::kV2, F::kV3);
  a.FCvt(FloatWidth::k64, FloatWidth::k32, F::kV0, F::kV1);
  a.Scvtf(FloatWidth::k64, F::kV2, k64, R::kX3);
  a.Fcvtzs(k32, R::kX0, FloatWidth::k32, F::kV1);
  a.FMovFromGeneral(FloatWidth::k64, F::kV6, R::kX7);

  const std::vector<uint32_t> expected = {
      0x1e222820, 0x1e652883, 0x1e632040, 0x1e22c020,
      0x9e620062, 0x1e380020, 0x9e6700e6,
  };
  WOLANG_EXPECT_EQ(a.words().size(), expected.size());
  for (size_t i = 0; i < expected.size() && i < a.words().size(); ++i) {
    WOLANG_EXPECT_EQ(a.words()[i], expected[i]);
  }
}

WOLANG_TEST(AssemblerTest, PicksTheShortestConstantSequence) {
  // A fixed width encoding cannot hold a 64-bit constant, so a constant
  // costs between one and four words. Which it is matters: materializing
  // every small number in four words would double the size of the loops
  // this tier exists to make fast.
  {
    Assembler a;
    a.MovImmediate(k64, R::kX0, 0);
    WOLANG_EXPECT_EQ(a.words().size(), 1u);
    WOLANG_EXPECT_EQ(a.words()[0], 0xd2800000u);
  }
  {
    // Every bit set is one `movn`, not four `movk`s.
    Assembler a;
    a.MovImmediate(k64, R::kX0, ~uint64_t{0});
    WOLANG_EXPECT_EQ(a.words().size(), 1u);
    WOLANG_EXPECT_EQ(a.words()[0], 0x92800000u);
  }
  {
    Assembler a;
    a.MovImmediate(k64, R::kX0, static_cast<uint64_t>(-2));
    WOLANG_EXPECT_EQ(a.words().size(), 1u);
    WOLANG_EXPECT_EQ(a.words()[0], 0x92800020u);
  }
  {
    Assembler a;
    a.MovImmediate(k32, R::kX1, 0x12345678u);
    WOLANG_EXPECT_EQ(a.words().size(), 2u);
    WOLANG_EXPECT_EQ(a.words()[0], 0x528acf01u);
    WOLANG_EXPECT_EQ(a.words()[1], 0x72a24681u);
  }
  {
    // A chunk that is zero is skipped rather than written.
    Assembler a;
    a.MovImmediate(k64, R::kX2, uint64_t{0xbeef} << 16);
    WOLANG_EXPECT_EQ(a.words().size(), 1u);
    WOLANG_EXPECT_EQ(a.words()[0], 0xd2b7dde2u);
  }
  {
    Assembler a;
    a.MovImmediate(k64, R::kX3, 0x4444333322221111ull);
    WOLANG_EXPECT_EQ(a.words().size(), 4u);
    WOLANG_EXPECT_EQ(a.words()[0], 0xd2822223u);
    WOLANG_EXPECT_EQ(a.words()[3], 0xf2e88883u);
  }
}

WOLANG_TEST(AssemblerTest, PatchesBranchesToALabel) {
  Assembler a;
  const Label top = a.CreateLabel();
  a.Bind(top);
  for (int i = 0; i < 10; ++i) {
    a.Nop();
  }
  a.B(top);
  a.BCond(Condition::kEq, top);
  a.Cbz(k64, R::kX0, top);
  a.Cbnz(k32, R::kX1, top);
  a.Resolve();

  WOLANG_EXPECT_EQ(a.words()[10], 0x17fffff6u);
  WOLANG_EXPECT_EQ(a.words()[11], 0x54fffea0u);
  WOLANG_EXPECT_EQ(a.words()[12], 0xb4fffe80u);
  WOLANG_EXPECT_EQ(a.words()[13], 0x35fffe61u);
}

WOLANG_TEST(AssemblerTest, PatchesAForwardBranch) {
  Assembler a;
  const Label skip = a.CreateLabel();
  a.BCond(Condition::kNe, skip);
  a.Nop();
  a.Bind(skip);
  a.Ret();
  a.Resolve();
  // Two words forward, in the 19-bit field.
  WOLANG_EXPECT_EQ(a.words()[0], 0x54000041u);
  WOLANG_EXPECT_EQ(a.OffsetOf(skip), 8u);
}

WOLANG_TEST(AssemblerTest, ReachesANegativeOffsetThroughTheUnscaledForm) {
  // A frame laid out below the frame pointer needs a negative offset, which
  // the scaled encoding cannot hold.
  Assembler a;
  a.Ldr(k64, R::kX8, R::kX9, -8);
  a.Str(k32, R::kX10, R::kX11, -4);
  WOLANG_EXPECT_EQ(a.words()[0], 0xf85f8128u);
  WOLANG_EXPECT_EQ(a.words()[1], 0xb81fc16au);
}

WOLANG_TEST(AssemblerTest, InvertsEveryConditionButAlways) {
  WOLANG_EXPECT_EQ(static_cast<int>(Invert(Condition::kEq)),
                   static_cast<int>(Condition::kNe));
  WOLANG_EXPECT_EQ(static_cast<int>(Invert(Condition::kLt)),
                   static_cast<int>(Condition::kGe));
  WOLANG_EXPECT_EQ(static_cast<int>(Invert(Condition::kGt)),
                   static_cast<int>(Condition::kLe));
  WOLANG_EXPECT_EQ(static_cast<int>(Invert(Condition::kAl)),
                   static_cast<int>(Condition::kAl));
}

}  // namespace
}  // namespace arm64
}  // namespace codegen
}  // namespace wolang
