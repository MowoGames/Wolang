// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/compiler.h"

#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/compiler/database.h"

namespace wolang {
namespace codegen {
namespace {

// Compiles a snippet and runs it in every tier there is.
//
// The interpreter on the unoptimized IR is the reference semantics; the
// interpreter on the optimized IR says whether the passes preserved
// meaning; the compiled code says whether this tier agrees with both. A
// disagreement is a miscompile, which is the worst bug a compiler can have,
// so every test here checks all three rather than only the answer.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  std::string Run(std::string_view name,
                  std::vector<interp::Value> arguments = {}) {
    const LoweredFile* lowered = db_.Lowered(file_);
    if (lowered == nullptr) {
      return "<not lowered>";
    }
    if (!lowered->problems.empty()) {
      return "<verifier: " + lowered->problems.front() + ">";
    }
    // The checker's resolution, not the file's own: an imported
    // signature only exists after the merge, and the code generator needs
    // the same view the checker and lowering had.
    const CheckedFile* checked = db_.Checked(file_);
    if (checked == nullptr) {
      return "<not checked>";
    }
    const FileResolution* resolution = &checked->resolution;

    const Symbol symbol = db_.interner().Intern(name);
    const interp::Outcome reference =
        interp::RunNamed(db_.interner(), db_.types(), lowered->unoptimized,
                         symbol, arguments);
    const interp::Outcome optimized =
        interp::RunNamed(db_.interner(), db_.types(), lowered->module,
                         symbol, arguments);
    if (Describe(reference) != Describe(optimized)) {
      return "<the passes changed the meaning: " + Describe(reference) +
             " then " + Describe(optimized) + ">";
    }

    if (compiled_ == nullptr) {
      compiled_ = Compile(db_.interner(), db_.types(), *resolution,
                          lowered->module);
    }
    if (!compiled_->Contains(symbol)) {
      return "<skipped: " + std::string(compiled_->SkipReason(symbol)) +
             ">";
    }
    const interp::Outcome machine = compiled_->Run(symbol, arguments);
    if (Describe(machine) != Describe(reference)) {
      return "<the code generator disagrees: interpreter said " +
             Describe(reference) + ", machine code said " +
             Describe(machine) + ">";
    }
    return Describe(machine);
  }

  // The same as Run, with the callers a trap walked back through.
  std::string RunWithBacktrace(std::string_view name,
                               std::vector<interp::Value> arguments,
                               std::vector<std::string>* backtrace) {
    const std::string first = Run(name, arguments);
    if (compiled_ == nullptr) {
      return first;
    }
    const Symbol symbol = db_.interner().Intern(name);
    const interp::Outcome outcome =
        compiled_->Run(symbol, arguments, backtrace);
    return Describe(outcome);
  }

  // Why the dev tier left a function alone, which is as much a part of its
  // behaviour as what it compiles.
  std::string Skipped(std::string_view name) {
    const LoweredFile* lowered = db_.Lowered(file_);
    const CheckedFile* checked = db_.Checked(file_);
    if (lowered == nullptr || checked == nullptr) {
      return "<not lowered>";
    }
    const FileResolution* resolution = &checked->resolution;
    if (compiled_ == nullptr) {
      compiled_ = Compile(db_.interner(), db_.types(), *resolution,
                          lowered->module);
    }
    const Symbol symbol = db_.interner().Intern(name);
    return std::string(compiled_->SkipReason(symbol));
  }

  static std::string Describe(const interp::Outcome& outcome) {
    if (outcome.ok()) {
      return outcome.value.Describe();
    }
    return "trap: " + std::string(ir::TrapKindName(outcome.trap)) + " at " +
           outcome.where + ": " + outcome.message;
  }

  static interp::Value Int(int64_t value) {
    return interp::Value::Int(static_cast<uint64_t>(value), IntWidth::k32,
                              true);
  }
  static interp::Value Wide(int64_t value) {
    return interp::Value::Int(static_cast<uint64_t>(value), IntWidth::k64,
                              true);
  }
  static interp::Value Byte(int64_t value) {
    return interp::Value::Int(static_cast<uint64_t>(value), IntWidth::k8,
                              true);
  }
  static interp::Value Unsigned(uint64_t value) {
    return interp::Value::Int(value, IntWidth::k32, false);
  }
  static interp::Value Float(double value) {
    return interp::Value::Float(value, FloatWidth::k32);
  }
  static interp::Value Double(double value) {
    return interp::Value::Float(value, FloatWidth::k64);
  }
  static interp::Value Bool(bool value) {
    return interp::Value::Bool(value);
  }

 private:
  Database db_;
  FileId file_;
  std::unique_ptr<CompiledModule> compiled_;
};

WOLANG_TEST(CodegenTest, RunsArithmetic) {
  Fixture f("fun Add(a: int, b: int) -> int => a + b\n");
  WOLANG_EXPECT_EQ(f.Run("Add", {Fixture::Int(2), Fixture::Int(3)}), "5");
  WOLANG_EXPECT_EQ(f.Run("Add", {Fixture::Int(-7), Fixture::Int(4)}), "-3");
}

WOLANG_TEST(CodegenTest, RunsEveryIntegerOperation) {
  Fixture f(R"(
fun Ops(a: int, b: int) -> int {
  var total = a + b
  total = total - 1
  total = total * 2
  total = total / 3
  total = total % 7
  total = total & 255
  total = total | 8
  total = total ^ 1
  total = total << 2
  total = total >> 1
  return -total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Ops", {Fixture::Int(10), Fixture::Int(5)}),
                   f.Run("Ops", {Fixture::Int(10), Fixture::Int(5)}));
  WOLANG_EXPECT_EQ(f.Run("Ops", {Fixture::Int(10), Fixture::Int(5)}), "-22");
}

WOLANG_TEST(CodegenTest, RunsControlFlow) {
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

WOLANG_TEST(CodegenTest, RunsALoopThroughSlots) {
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
  WOLANG_EXPECT_EQ(f.Run("Sum", {Fixture::Int(100)}), "4950");
}

WOLANG_TEST(CodegenTest, RunsRecursionThroughCalls) {
  Fixture f(R"(
fun Fib(n: int) -> int {
  if n < 2 {
    return n
  }
  return Fib(n - 1) + Fib(n - 2)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Fib", {Fixture::Int(10)}), "55");
  WOLANG_EXPECT_EQ(f.Run("Fib", {Fixture::Int(20)}), "6765");
}

WOLANG_TEST(CodegenTest, PassesBlockParametersAcrossEdges) {
  Fixture f(R"(
fun Pick(c: bool) -> int {
  let v = if c { 10 } else { 20 }
  return v
}
)");
  WOLANG_EXPECT_EQ(f.Run("Pick", {Fixture::Bool(true)}), "10");
  WOLANG_EXPECT_EQ(f.Run("Pick", {Fixture::Bool(false)}), "20");
}

WOLANG_TEST(CodegenTest, TrapsTheSameWayTheInterpreterDoes) {
  // The whole point of the trap table: the kind, the place and the message
  // are the interpreter's, so the two tiers' outcomes compare in full.
  Fixture f(R"(
fun Divide(a: int, b: int) -> int => a / b

fun Overflow(a: i8, b: i8) -> i8 => a + b

fun Shift(a: int, b: int) -> int => a << b
)");
  WOLANG_EXPECT_EQ(f.Run("Divide", {Fixture::Int(6), Fixture::Int(3)}), "2");
  WOLANG_EXPECT_EQ(
      f.Run("Divide", {Fixture::Int(6), Fixture::Int(0)}),
      "trap: division by zero at @Divide:%2: divided by zero");
  WOLANG_EXPECT_EQ(f.Run("Overflow", {Fixture::Byte(100),
                                      Fixture::Byte(100)}),
                   "trap: overflow at @Overflow:%2: add overflowed");
  WOLANG_EXPECT_EQ(f.Run("Overflow", {Fixture::Byte(100),
                                      Fixture::Byte(27)}),
                   "127");
  WOLANG_EXPECT_EQ(f.Run("Shift", {Fixture::Int(1), Fixture::Int(40)}),
                   "trap: overflow at @Shift:%2: "
                   "shifted by more than the width");
}

WOLANG_TEST(CodegenTest, RunsFloatingPoint) {
  Fixture f(R"(
fun Mix(a: float, b: float) -> float => a * b + a / b - a

fun Wide(a: f64, b: f64) -> f64 => a / b

fun Compare(a: float, b: float) -> bool => a < b
)");
  WOLANG_EXPECT_EQ(f.Run("Mix", {Fixture::Float(3.0), Fixture::Float(2.0)}),
                   "4.5");
  WOLANG_EXPECT_EQ(f.Run("Wide", {Fixture::Double(1.0),
                                  Fixture::Double(3.0)}),
                   "0.333333");
  WOLANG_EXPECT_EQ(f.Run("Compare", {Fixture::Float(1.0),
                                     Fixture::Float(2.0)}),
                   "true");
  // Division by zero is an infinity, not a fault.
  WOLANG_EXPECT_EQ(f.Run("Wide", {Fixture::Double(1.0),
                                  Fixture::Double(0.0)}),
                   "inf");
}

WOLANG_TEST(CodegenTest, ConvertsWithAs) {
  Fixture f(R"(
fun Widen(n: i32) -> i64 => n as i64

fun Narrow(n: i32) -> u8 => n as u8

fun ToFloat(n: int) -> float => n as float

fun Truncate(x: float) -> int => x as int
)");
  WOLANG_EXPECT_EQ(f.Run("Widen", {Fixture::Int(-5)}), "-5");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {Fixture::Int(200)}), "200");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {Fixture::Int(300)}),
                   "trap: overflow at @Narrow:%1: "
                   "the value does not fit the integer it was cast to");
  WOLANG_EXPECT_EQ(f.Run("ToFloat", {Fixture::Int(3)}), "3");
  WOLANG_EXPECT_EQ(f.Run("Truncate", {Fixture::Float(-2.75)}), "-2");
}

WOLANG_TEST(CodegenTest, RunsStructsThroughTheFrame) {
  Fixture f(R"(
struct Vec2 {
  var x: float
  var y: float
}

fun Dot(a: Vec2, b: Vec2) -> float => a.x * b.x + a.y * b.y

fun Scale(v: Vec2, k: float) -> Vec2 {
  return Vec2(v.x * k, v.y * k)
}
)");
  const interp::Value a = interp::Value::Aggregate(
      {Fixture::Float(1.0), Fixture::Float(2.0)});
  const interp::Value b = interp::Value::Aggregate(
      {Fixture::Float(3.0), Fixture::Float(4.0)});
  WOLANG_EXPECT_EQ(f.Run("Dot", {a, b}), "11");
  WOLANG_EXPECT_EQ(f.Run("Scale", {a, Fixture::Float(2.0)}), "(2, 4)");
}

WOLANG_TEST(CodegenTest, TrapsOutsideAnArray) {
  Fixture f(R"(
fun At(i: int) -> int {
  let xs = [10, 20, 30]
  return xs[i]
}
)");
  WOLANG_EXPECT_EQ(f.Run("At", {Fixture::Int(1)}), "20");
  WOLANG_EXPECT_EQ(f.Run("At", {Fixture::Int(3)}),
                   "trap: out of bounds at @At:%5: "
                   "index is outside a length of 3");
  WOLANG_EXPECT_EQ(f.Run("At", {Fixture::Int(-1)}),
                   "trap: out of bounds at @At:%5: "
                   "index is outside a length of 3");
}

WOLANG_TEST(CodegenTest, RunsOptionals) {
  Fixture f(R"(
fun Unwrap(p: int?) -> int {
  if let v = p {
    return v
  }
  return 0
}
)");
  WOLANG_EXPECT_EQ(f.Run("Unwrap", {interp::Value::Present(
                                        Fixture::Int(7))}),
                   "7");
  WOLANG_EXPECT_EQ(f.Run("Unwrap", {interp::Value::Absent()}), "0");
}

WOLANG_TEST(CodegenTest, NamesTheCallersOfATrap) {
  // A frame record in every compiled function, including the leaf, is what
  // makes this possible: the walk is two loads per frame and needs no table
  // at all.
  Fixture f(R"(
fun Down(n: int) -> int {
  if n == 0 {
    return 1 / n
  }
  return Down(n - 1)
}
)");
  std::vector<std::string> frames;
  WOLANG_EXPECT_EQ(f.RunWithBacktrace("Down", {Fixture::Int(3)}, &frames),
                   "trap: division by zero at @Down:%5: divided by zero");
  // Three callers, each of them `Down`, and nothing above the trampoline.
  WOLANG_EXPECT_EQ(frames.size(), 3u);
  for (const std::string& frame : frames) {
    WOLANG_EXPECT_EQ(frame.substr(0, 6), "@Down+");
  }
}

WOLANG_TEST(CodegenTest, SaysWhatItCannotCompile) {
  // A tier that compiles what it can has to say what it did not, or "why
  // is this slow" has no answer.
  Fixture f(R"(
fun Greet(name: str) -> str => name

fun Count(n: int) -> int => n + 1
)");
  WOLANG_EXPECT_EQ(f.Skipped("Greet"), "takes `str`");
  WOLANG_EXPECT_EQ(f.Skipped("Count"), "");
}

WOLANG_TEST(CodegenTest, SpillsWhenThereAreMoreValuesThanRegisters) {
  // Nine integer registers, and this holds more than nine values at once.
  Fixture f(R"(
fun Many(a: int, b: int, c: int, d: int, e: int) -> int {
  let p = a + b
  let q = b + c
  let r = c + d
  let s = d + e
  let t = e + a
  let u = p + q
  let v = q + r
  let w = r + s
  let x = s + t
  let y = t + u
  let z = u + v
  return p + q + r + s + t + u + v + w + x + y + z
}
)");
  WOLANG_EXPECT_EQ(f.Run("Many", {Fixture::Int(1), Fixture::Int(2),
                                  Fixture::Int(3), Fixture::Int(4),
                                  Fixture::Int(5)}),
                   "115");
}

WOLANG_TEST(CodegenTest, CompilesAGenericThroughMonomorphization) {
  // A generic function has no code until something instantiates it, so
  // this tier never sees a `T`: it compiles the copy, and the copy is as
  // concrete as anything else.
  Fixture f(R"(
fun Larger<T>(a: T, b: T) -> T {
  if a > b {
    return a
  }
  return b
}

fun PickInt(a: int, b: int) -> int => Larger(a, b)

fun PickFloat(a: float, b: float) -> float => Larger(a, b)
)");
  WOLANG_EXPECT_EQ(f.Run("PickInt", {Fixture::Int(3), Fixture::Int(9)}),
                   "9");
  WOLANG_EXPECT_EQ(f.Run("PickFloat", {Fixture::Float(2.5),
                                       Fixture::Float(1.5)}),
                   "2.5");
  WOLANG_EXPECT_EQ(f.Skipped("Larger<i32>"), "");
  WOLANG_EXPECT_EQ(f.Skipped("Larger<f32>"), "");
}

}  // namespace
}  // namespace codegen
}  // namespace wolang
