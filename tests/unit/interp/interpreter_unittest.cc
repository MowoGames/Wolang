// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/interp/interpreter.h"

#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/abi/layout.h"
#include "wolang/compiler/database.h"

namespace wolang {
namespace {

// Compiles a snippet and runs it, through both the unoptimized and the
// optimized IR. Every test checks that the two agree: a difference is a
// miscompile, which is the worst bug this compiler can have, and checking it
// everywhere costs one more call.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  // The result of running |name|, or the trap that stopped it.
  std::string Run(std::string_view name, std::vector<interp::Value> args = {}) {
    const LoweredFile* lowered = db_.Lowered(file_);
    if (lowered == nullptr) {
      return "<not lowered>";
    }
    if (!lowered->problems.empty()) {
      return "<verifier: " + lowered->problems.front() + ">";
    }

    const Symbol symbol = db_.interner().Intern(name);
    const interp::Outcome reference =
        interp::RunNamed(db_.interner(), db_.types(), lowered->unoptimized,
                         symbol, args);
    const interp::Outcome optimized =
        interp::RunNamed(db_.interner(), db_.types(), lowered->module, symbol,
                         args);

    if (reference.trap != optimized.trap ||
        (reference.ok() && reference.value != optimized.value)) {
      return "<the passes changed the meaning: " + Describe(reference) +
             " then " + Describe(optimized) + ">";
    }
    return Describe(reference);
  }

  static std::string Describe(const interp::Outcome& outcome) {
    if (outcome.ok()) {
      return outcome.value.Describe();
    }
    return std::string("trap: ") +
           std::string(interp::TrapKindName(outcome.trap));
  }

  static interp::Value Int(int64_t value) {
    return interp::Value::Int(static_cast<uint64_t>(value), IntWidth::k32,
                              true);
  }

  static interp::Value Float(double value) {
    return interp::Value::Float(value, FloatWidth::k32);
  }

  static interp::Value Double(double value) {
    return interp::Value::Float(value, FloatWidth::k64);
  }

 private:
  Database db_;
  FileId file_;
};

WOLANG_TEST(InterpreterTest, RunsArithmetic) {
  Fixture f("fun Add(a: int, b: int) -> int => a + b\n");
  WOLANG_EXPECT_EQ(f.Run("Add", {Fixture::Int(2), Fixture::Int(3)}), "5");
}

WOLANG_TEST(InterpreterTest, RunsControlFlow) {
  Fixture f(R"(
fun Max(a: int, b: int) -> int {
  if a > b {
    return a
  }
  return b
}
)");
  WOLANG_EXPECT_EQ(f.Run("Max", {Fixture::Int(7), Fixture::Int(3)}), "7");
  WOLANG_EXPECT_EQ(f.Run("Max", {Fixture::Int(3), Fixture::Int(7)}), "7");
}

WOLANG_TEST(InterpreterTest, RunsALoopThroughSlots) {
  Fixture f(R"(
fun Sum(n: int) -> int {
  var total = 0
  var i = 0
  while i < n {
    total = total + i
    i = i + 1
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Sum", {Fixture::Int(5)}), "10");
  WOLANG_EXPECT_EQ(f.Run("Sum", {Fixture::Int(0)}), "0");
}

WOLANG_TEST(InterpreterTest, LabelledBreakAndContinueReachRangeForAndWhile) {
  // The label used to be read from the wrong field, so a labelled jump out of
  // a range-for or while never found its loop and hit `unreachable`.
  Fixture f(R"(
fun BreakFor() -> int {
  var n = 0
  outer: for i in 0..5 {
    for j in 0..5 {
      if j == 2 { break outer }
      n = n + 1
    }
  }
  return n
}
fun ContinueFor() -> int {
  var n = 0
  outer: for i in 0..3 {
    for j in 0..5 {
      if j == 2 { continue outer }
      n = n + 1
    }
  }
  return n
}
fun BreakWhile() -> int {
  var n = 0
  var i = 0
  outer: while i < 5 {
    i = i + 1
    for j in 0..5 {
      if j == 1 { break outer }
      n = n + 1
    }
  }
  return n
}
fun ContinueWhile() -> int {
  var n = 0
  var i = 0
  outer: while i < 3 {
    i = i + 1
    var j = 0
    while j < 5 {
      j = j + 1
      if j == 2 { continue outer }
      n = n + 1
    }
  }
  return n
}
)");
  WOLANG_EXPECT_EQ(f.Run("BreakFor"), "2");
  WOLANG_EXPECT_EQ(f.Run("ContinueFor"), "6");
  WOLANG_EXPECT_EQ(f.Run("BreakWhile"), "1");
  WOLANG_EXPECT_EQ(f.Run("ContinueWhile"), "3");
}

WOLANG_TEST(InterpreterTest, ConstantFoldingLeavesOverflowToTrap) {
  // Fixture::Run checks the optimized IR against the unoptimized one, so a
  // fold into the wrapped value (or a crash inside the compiler on
  // MIN / -1) shows up here (D224).
  Fixture f(R"(
fun AddS64() -> s64 {
  const a: s64 = 9223372036854775807
  const b: s64 = 1
  return a + b
}
fun AddU64() -> u64 {
  const a: u64 = 18446744073709551615
  const b: u64 = 1
  return a + b
}
fun SubU64() -> u64 {
  const a: u64 = 0
  const b: u64 = 1
  return a - b
}
fun MulS64() -> s64 {
  const a: s64 = 4611686018427387904
  const b: s64 = 2
  return a * b
}
fun MulU64() -> u64 {
  const a: u64 = 4294967296
  const b: u64 = 4294967296
  return a * b
}
fun DivMin() -> s64 {
  const m: s64 = -9223372036854775807
  const a: s64 = m - 1
  const b: s64 = -1
  return a / b
}
fun RemMin() -> s64 {
  const m: s64 = -9223372036854775807
  const a: s64 = m - 1
  const b: s64 = -1
  return a % b
}
fun DivMin8() -> s8 {
  const a: s8 = -128
  const b: s8 = -1
  return a / b
}
fun NegMin() -> s64 {
  const m: s64 = -9223372036854775807
  const a: s64 = m - 1
  return -a
}
fun NegMin8() -> s8 {
  const a: s8 = -128
  return -a
}
fun FineDiv() -> s64 {
  const a: s64 = -9223372036854775807
  const b: s64 = -1
  return a / b
}
fun FineNeg() -> s64 {
  const a: s64 = -9223372036854775807
  return -a
}
)");
  WOLANG_EXPECT_EQ(f.Run("AddS64"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("AddU64"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("SubU64"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("MulS64"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("MulU64"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("DivMin"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("RemMin"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("DivMin8"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("NegMin"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("NegMin8"), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("FineDiv"), "9223372036854775807");
  WOLANG_EXPECT_EQ(f.Run("FineNeg"), "9223372036854775807");
}

WOLANG_TEST(InterpreterTest, RunsRecursion) {
  Fixture f(R"(
fun Fib(n: int) -> int {
  if n < 2 {
    return n
  }
  return Fib(n - 1) + Fib(n - 2)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Fib", {Fixture::Int(10)}), "55");
}

WOLANG_TEST(InterpreterTest, PassesBlockParametersAcrossEdges) {
  // This is what a phi node would have had to work out backwards.
  Fixture f(R"(
fun Pick(c: bool) -> int {
  const v = if c { 10 } else { 20 }
  return v
}
)");
  WOLANG_EXPECT_EQ(f.Run("Pick", {interp::Value::Bool(true)}), "10");
  WOLANG_EXPECT_EQ(f.Run("Pick", {interp::Value::Bool(false)}), "20");
}

WOLANG_TEST(InterpreterTest, ShortCircuitsWithoutEvaluatingTheRightSide) {
  // `Boom` divides by zero, so reaching it would trap. `&&` must not.
  Fixture f(R"(
fun Boom() -> bool {
  const x = 1 / 0
  return true
}

fun Guarded(c: bool) -> bool {
  return c && Boom()
}
)");
  WOLANG_EXPECT_EQ(f.Run("Guarded", {interp::Value::Bool(false)}), "false");
  WOLANG_EXPECT_EQ(f.Run("Guarded", {interp::Value::Bool(true)}),
                   "trap: division by zero");
}

WOLANG_TEST(InterpreterTest, TrapsOnDivisionByZero) {
  Fixture f("fun Bad(n: int) -> int => n / 0\n");
  WOLANG_EXPECT_EQ(f.Run("Bad", {Fixture::Int(1)}),
                   "trap: division by zero");
}

WOLANG_TEST(InterpreterTest, TrapsOnOverflowRatherThanWrapping) {
  // A language that wraps silently turns an arithmetic bug into a wrong
  // answer somewhere else entirely.
  Fixture f(R"(
fun Grow(a: s8, b: s8) -> s8 {
  return a + b
}
)");
  const interp::Value big =
      interp::Value::Int(127, IntWidth::k8, /*is_signed=*/true);
  WOLANG_EXPECT_EQ(f.Run("Grow", {big, big}), "trap: overflow");

  const interp::Value small =
      interp::Value::Int(1, IntWidth::k8, /*is_signed=*/true);
  WOLANG_EXPECT_EQ(f.Run("Grow", {small, small}), "2");
}

WOLANG_TEST(InterpreterTest, TrapsOutsideAnArray) {
  Fixture f(R"(
fun Read(i: int) -> int {
  const xs: [3]int = [10, 20, 30]
  return xs[i]
}
)");
  WOLANG_EXPECT_EQ(f.Run("Read", {Fixture::Int(1)}), "20");
  WOLANG_EXPECT_EQ(f.Run("Read", {Fixture::Int(3)}), "trap: out of bounds");
}

WOLANG_TEST(InterpreterTest, TrapsOnAnEndlessLoopThroughTheStepLimit) {
  // Not a property of the program but of the run, and what keeps a
  // `comptime` loop from hanging the compiler.
  Fixture f(R"(
fun Spin() -> int {
  var i = 0
  loop {
    i = i + 1
  }
}
)");
  WOLANG_EXPECT_EQ(f.Run("Spin"), "trap: step limit");
}

WOLANG_TEST(InterpreterTest, RunsOptionals) {
  Fixture f(R"(
struct Pawn {
  var health: float
}

fun Read(p: Pawn?) -> float {
  return p?.health ?? -1.0
}

fun Make(h: float) -> Pawn? {
  return Pawn(h)
}

fun Absent() -> Pawn? {
  return null
}

fun RoundTrip(h: float) -> float {
  return Read(Make(h))
}

fun Missing() -> float {
  return Read(Absent())
}
)");
  WOLANG_EXPECT_EQ(f.Run("RoundTrip", {interp::Value::Float(
                                          3.5, FloatWidth::k32)}),
                   "3.5");
  WOLANG_EXPECT_EQ(f.Run("Missing"), "-1");
}

WOLANG_TEST(InterpreterTest, RunsAMatch) {
  Fixture f(R"(
fun Classify(n: int) -> int {
  return match n {
    0 => 100
    1 => 200
    _ => 300
  }
}
)");
  WOLANG_EXPECT_EQ(f.Run("Classify", {Fixture::Int(0)}), "100");
  WOLANG_EXPECT_EQ(f.Run("Classify", {Fixture::Int(1)}), "200");
  WOLANG_EXPECT_EQ(f.Run("Classify", {Fixture::Int(9)}), "300");
}

WOLANG_TEST(InterpreterTest, RunsStructsAndFields) {
  Fixture f(R"(
struct Vec3 {
  var x: float
  var y: float
}

fun Length(v: Vec3) -> float {
  return v.x + v.y
}

fun Use() -> float {
  return Length(Vec3(1.5, 2.5))
}
)");
  WOLANG_EXPECT_EQ(f.Run("Use"), "4");
}

WOLANG_TEST(InterpreterTest, FloatsRoundAtTheirDeclaredWidth) {
  // Doing f32 arithmetic at double precision would make the interpreter
  // disagree with every other tier.
  Fixture f(R"(
fun Third() -> float {
  return 1.0 / 3.0
}

fun Wide() -> f64 {
  return 1.0 / 3.0
}
)");
  WOLANG_EXPECT_EQ(f.Run("Third"), "0.333333");
  WOLANG_EXPECT_EQ(f.Run("Wide"), "0.333333");
  // The two differ in their bits even though both print rounded, which is
  // what the width is for; the check that matters is that neither traps.
}

WOLANG_TEST(InterpreterTest, ConvertsBetweenNumbersWithAs) {
  // `as` is the only numeric conversion the language has, so it has to do
  // something: before this it was lowered to nothing and the width never
  // changed.
  Fixture f(R"(
fun Widen(n: s32) -> s64 {
  return n as s64
}

fun ToFloat(n: int) -> float {
  return n as float
}

fun Truncate(x: float) -> int {
  return x as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("Widen", {Fixture::Int(7)}), "7");
  WOLANG_EXPECT_EQ(f.Run("ToFloat", {Fixture::Int(3)}), "3");
  WOLANG_EXPECT_EQ(f.Run("Truncate", {Fixture::Float(2.75)}), "2");
  WOLANG_EXPECT_EQ(f.Run("Truncate", {Fixture::Float(-2.75)}), "-2");
}

WOLANG_TEST(InterpreterTest, ANarrowingCastTrapsRatherThanTruncating) {
  // Overflow traps everywhere else in this language; a cast that quietly
  // kept the low bits would be the one place a number changes silently.
  Fixture f(R"(
fun Narrow(n: s32) -> u8 {
  return n as u8
}

fun Huge(x: f64) -> s32 {
  return x as s32
}
)");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {Fixture::Int(200)}), "200");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {Fixture::Int(300)}), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {Fixture::Int(-1)}), "trap: overflow");
  WOLANG_EXPECT_EQ(f.Run("Huge", {Fixture::Double(1e18)}),
                   "trap: overflow");
}

WOLANG_TEST(InterpreterTest, UnitsHaveNoRuntimeCost) {
  Fixture f(R"(
dimension Length
dimension Time
unit m: Length
unit s: Time

fun Travel(d: Length, t: Time) -> Length {
  return d
}

fun Use() -> Length {
  return Travel(3m, 1s)
}
)");
  // The dimension is gone, so the value is just the number.
  WOLANG_EXPECT_EQ(f.Run("Use"), "3");
}

WOLANG_TEST(ValueTest, MasksToItsDeclaredWidth) {
  const interp::Value small =
      interp::Value::Int(0x1FF, IntWidth::k8, /*is_signed=*/false);
  WOLANG_EXPECT_EQ(small.integer(), uint64_t{0xFF});

  const interp::Value negative =
      interp::Value::Int(0xFF, IntWidth::k8, /*is_signed=*/true);
  WOLANG_EXPECT_EQ(negative.signed_integer(), int64_t{-1});
  WOLANG_EXPECT_EQ(negative.Describe(), "-1");
}

WOLANG_TEST(ValueTest, ComparesAggregatesStructurally) {
  const interp::Value a = interp::Value::Aggregate(
      {interp::Value::Bool(true), interp::Value::Str("x")});
  const interp::Value b = interp::Value::Aggregate(
      {interp::Value::Bool(true), interp::Value::Str("x")});
  const interp::Value c = interp::Value::Aggregate(
      {interp::Value::Bool(false), interp::Value::Str("x")});

  WOLANG_EXPECT_EQ(a, b);
  WOLANG_EXPECT_NE(a, c);
}

WOLANG_TEST(ValueTest, AnAbsentOptionalIsNotAPresentOne) {
  WOLANG_EXPECT_NE(interp::Value::Absent(),
                   interp::Value::Present(interp::Value::Bool(false)));
  WOLANG_EXPECT_EQ(interp::Value::Absent().Describe(), "null");
}


// ----------------------------------------------------------- comptime -----

WOLANG_TEST(ComptimeTest, EvaluatesAFunctionWithNoEffects) {
  // A `comptime` function has no effects -- wave 5 checks that -- so it can
  // be run inside the compiler.
  Database db;
  const FileId file =
      db.AddFile("<test>", "comptime fun Fine() -> int {\n  return 2 + 2\n}\n");
  const std::vector<Diagnostic>* diagnostics = db.Diagnostics(file);
  WOLANG_ASSERT_NE(diagnostics, nullptr);
  WOLANG_EXPECT(diagnostics->empty());
}

WOLANG_TEST(ComptimeTest, ATrapBecomesADiagnostic) {
  Database db;
  const FileId file = db.AddFile(
      "<test>", "comptime fun Broken() -> int {\n  return 1 / 0\n}\n");
  const std::vector<Diagnostic>* diagnostics = db.Diagnostics(file);
  WOLANG_ASSERT_NE(diagnostics, nullptr);
  WOLANG_ASSERT_EQ(diagnostics->size(), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics->front().code, "E0901");
  WOLANG_EXPECT(diagnostics->front().message.find("division by zero") !=
                std::string::npos);
}

WOLANG_TEST(ComptimeTest, AnEndlessEvaluationStopsBeingTheCompilersProblem) {
  Database db;
  const FileId file = db.AddFile(
      "<test>",
      "comptime fun Endless() -> int {\n  var i = 0\n"
      "  loop { i = i + 1 }\n}\n");
  const std::vector<Diagnostic>* diagnostics = db.Diagnostics(file);
  WOLANG_ASSERT_NE(diagnostics, nullptr);
  WOLANG_ASSERT_EQ(diagnostics->size(), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics->front().code, "E0901");
}

WOLANG_TEST(ComptimeTest, AFunctionWithArgumentsIsLeftForItsCallSite) {
  // Evaluating one needs the arguments, which means the call site; nothing
  // is reported here rather than something wrong.
  Database db;
  const FileId file = db.AddFile(
      "<test>", "comptime fun Twice(n: int) -> int => n + n\n");
  const std::vector<Diagnostic>* diagnostics = db.Diagnostics(file);
  WOLANG_ASSERT_NE(diagnostics, nullptr);
  WOLANG_EXPECT(diagnostics->empty());
}

WOLANG_TEST(ComptimeTest, ACallWithConstantArgumentsBecomesAConstant) {
  // The other half of D103: the call site is where one with arguments is
  // evaluated, and what is left is the constant it worked out to -- in
  // every tier, because the fold happens before the passes.
  Database db;
  const FileId file = db.AddFile(
      "<test>",
      "comptime fun Fib(n: int) -> int {\n"
      "  if n < 2 {\n    return n\n  }\n"
      "  return Fib(n - 1) + Fib(n - 2)\n}\n"
      "fun Probe() -> int {\n  return Fib(20) + 1\n}\n");
  const LoweredFile* lowered = db.Lowered(file);
  WOLANG_ASSERT_NE(lowered, nullptr);
  const ir::Function* probe =
      lowered->module.Find(db.interner().Intern("Probe"));
  WOLANG_ASSERT_NE(probe, nullptr);
  bool calls_anything = false;
  uint64_t folded = 0;
  for (const ir::Inst& inst : probe->instructions) {
    calls_anything |= inst.opcode == ir::Opcode::kCall;
    if (inst.opcode == ir::Opcode::kConstInt) {
      folded = std::max(folded, inst.payload.integer);
    }
  }
  WOLANG_EXPECT(!calls_anything);
  // The twentieth Fibonacci number, and the `+ 1` folded with it.
  WOLANG_EXPECT_EQ(folded, uint64_t{6766});
}

WOLANG_TEST(ComptimeTest, ATrapInSuchACallIsReportedToo) {
  Database db;
  const FileId file = db.AddFile(
      "<test>",
      "comptime fun Divide(a: int, b: int) -> int => a / b\n"
      "fun Probe() -> int {\n  return Divide(10, 0)\n}\n");
  const std::vector<Diagnostic>* diagnostics = db.Diagnostics(file);
  WOLANG_ASSERT_NE(diagnostics, nullptr);
  WOLANG_ASSERT_EQ(diagnostics->size(), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics->front().code, "E0901");
  WOLANG_EXPECT(diagnostics->front().message.find("division by zero") !=
                std::string::npos);
}

WOLANG_TEST(TestAttributeTest, CollectsFunctionsMarkedTest) {
  Database db;
  const FileId file = db.AddFile(
      "<test>",
      "@Test\nfun Passes() -> bool => true\n"
      "fun Ordinary() -> bool => true\n"
      "@Test\nfun AlsoATest() -> bool => true\n");

  const LoweredFile* lowered = db.Lowered(file);
  WOLANG_ASSERT_NE(lowered, nullptr);
  WOLANG_ASSERT_EQ(lowered->tests.size(), size_t{2});
  WOLANG_EXPECT_EQ(db.interner().Text(lowered->tests[0]), "Passes");
  WOLANG_EXPECT_EQ(db.interner().Text(lowered->tests[1]), "AlsoATest");
}


// --------------------------------------------------------- intrinsics -----

// Runs `main` and returns what it printed. The host side of the platform
// floor: everything the language can say about itself is written in Wolang,
// and this is what is left.
class ProgramFixture {
 public:
  explicit ProgramFixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  std::string Output() {
    const LoweredFile* lowered = db_.Lowered(file_);
    if (lowered == nullptr) {
      return "<not lowered>";
    }
    if (!lowered->problems.empty()) {
      return "<verifier: " + lowered->problems.front() + ">";
    }

    // A string is a pointer and a length now, so reading one needs the
    // layout: where a field of an aggregate is, is the one question the
    // interpreter cannot answer by itself.
    const CheckedFile* checked = db_.Checked(file_);
    if (checked == nullptr) {
      return "<not checked>";
    }
    abi::LayoutStore layouts(db_.types(), checked->resolution);

    const Symbol main = db_.interner().Intern("main");
    interp::Output reference;
    interp::Output optimized;
    const interp::Outcome a = interp::RunNamed(
        db_.interner(), db_.types(), lowered->unoptimized, main, {}, {},
        &reference, nullptr, &layouts);
    const interp::Outcome b = interp::RunNamed(
        db_.interner(), db_.types(), lowered->module, main, {}, {},
        &optimized, nullptr, &layouts);
    if (!a.ok()) {
      return "trap: " + std::string(interp::TrapKindName(a.trap));
    }
    if (a.trap != b.trap || reference.text != optimized.text) {
      return "<the passes changed what it printed>";
    }
    return reference.text;
  }

 private:
  Database db_;
  FileId file_;
};

WOLANG_TEST(IntrinsicTest, PrintsAString) {
  ProgramFixture f(R"(
fun main() uses io {
  Print("hello")
}

offer fun Print(message: text) uses io
)");
  WOLANG_EXPECT_EQ(f.Output(), "hello\n");
}

WOLANG_TEST(IntrinsicTest, BuildsAnInterpolatedString) {
  // Each piece is converted by an intrinsic and the results concatenated,
  // which is the one thing here the language cannot say about itself.
  ProgramFixture f(R"(
fun main() uses io {
  const n = 42
  const f = 1.5
  const b = true
  Print("n={n} f={f} b={b} and {n}")
}

offer fun Print(message: text) uses io
)");
  WOLANG_EXPECT_EQ(f.Output(), "n=42 f=1.5 b=true and 42\n");
}

WOLANG_TEST(IntrinsicTest, AnOwnedStringIsAcceptedWhereABorrowedOneIsWanted) {
  // An interpolated string is owned and `Print` takes a borrow. Without the
  // implicit borrow the most common operation in any language would not
  // type check.
  ProgramFixture f(R"(
fun main() uses io {
  const who = "world"
  Print("hello, {who}")
}

offer fun Print(message: text) uses io
)");
  WOLANG_EXPECT_EQ(f.Output(), "hello, world\n");
}

WOLANG_TEST(IntrinsicTest, AnUnsignedIntegerInterpolatesWithoutGoingNegative) {
  // It used to be widened to s64 first, so 2^63 and up printed negative or
  // trapped (D224).
  ProgramFixture f(R"(
fun main() uses io {
  const big: u64 = 18446744073709551615
  const small: u8 = 200
  const neg: s64 = -5
  Print("{big} {small} {neg}")
}

offer fun Print(message: text) uses io
)");
  WOLANG_EXPECT_EQ(f.Output(), "18446744073709551615 200 -5\n");
}

WOLANG_TEST(IntrinsicTest, FloatRemainderFollowsFmod) {
  // `%` on floats type checked but had no implementation in any tier (D224).
  ProgramFixture f(R"(
fun main() uses io {
  const a: float = 5.5
  const b: float = -5.5
  const two: float = 2.0
  Print("{a % two} {b % two} {a % -two}")
}

offer fun Print(message: text) uses io
)");
  WOLANG_EXPECT_EQ(f.Output(), "1.5 -1.5 1.5\n");
}

WOLANG_TEST(IntrinsicTest, ADeclarationThatIsNotAnIntrinsicTraps) {
  ProgramFixture f(R"(
fun main() {
  const _x = Nowhere()
}

offer fun Nowhere() -> int
)");
  WOLANG_EXPECT_EQ(f.Output(), "trap: no body");
}

}  // namespace
}  // namespace wolang
