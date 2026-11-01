// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/compiler.h"

#include <cstdio>
#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/codegen/foreign.h"
#include "wolang/runtime/scheduler.h"
#include "wolang/abi/layout.h"
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
    if (foreign_ == nullptr) {
      foreign_ = codegen::MakeForeignCaller();
    }
    abi::LayoutStore layouts(db_.types(), *resolution);
    InstallHost(&layouts, lowered);
    const interp::Outcome reference =
        interp::RunNamed(db_.interner(), db_.types(), lowered->unoptimized,
                         symbol, arguments, {}, nullptr, foreign_.get(),
                         &layouts);
    const interp::Outcome optimized =
        interp::RunNamed(db_.interner(), db_.types(), lowered->module,
                         symbol, arguments, {}, nullptr, foreign_.get(),
                         &layouts);
    if (!Agree(reference, optimized)) {
      return "<the passes changed the meaning: " + Describe(reference) +
             " then " + Describe(optimized) + ">";
    }

    if (compiled_ == nullptr) {
      compiled_ = Compile(db_.interner(), db_.types(), *resolution,
                          lowered->module);
    }
    // A test may spawn, so the scheduler needs its way back into whichever
    // tier is running. Both tiers go through the same door.
    InstallHost(&layouts, lowered);
    host_.compiled = true;
    if (!compiled_->Contains(symbol)) {
      return "<skipped: " + std::string(compiled_->SkipReason(symbol)) +
             ">";
    }
    const interp::Outcome machine = compiled_->Run(symbol, arguments);
    if (!Agree(reference, machine)) {
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

  // What the scheduler resumes through, for whichever tier is asked.
  struct Host {
    Fixture* fixture = nullptr;
    const LoweredFile* lowered = nullptr;
    abi::LayoutStore* layouts = nullptr;
    bool compiled = false;
  };

  static bool Resume(void* context, int32_t kind, void* frame) {
    auto* host = static_cast<Host*>(context);
    Fixture& fixture = *host->fixture;
    const Symbol dispatch = fixture.db_.interner().Intern("wolang$dispatch");
    if (host->compiled && fixture.compiled_ != nullptr &&
        fixture.compiled_->Contains(dispatch)) {
      bool faulted = false;
      return fixture.compiled_->ResumeTask(dispatch, kind, frame, &faulted);
    }
    std::vector<interp::Value> arguments;
    arguments.push_back(interp::Value::Int(static_cast<uint64_t>(kind),
                                           IntWidth::k32, true));
    arguments.push_back(interp::Value::Int(
        reinterpret_cast<uint64_t>(frame), IntWidth::k64, false));
    const interp::Outcome outcome = interp::RunNamed(
        fixture.db_.interner(), fixture.db_.types(), host->lowered->module,
        dispatch, arguments, {}, nullptr, fixture.foreign_.get(),
        host->layouts);
    return !outcome.ok() ||
           (outcome.value.kind() == interp::Value::Kind::kBool &&
            outcome.value.boolean());
  }

  void InstallHost(abi::LayoutStore* layouts, const LoweredFile* lowered) {
    host_.fixture = this;
    host_.lowered = lowered;
    host_.layouts = layouts;
    host_.compiled = false;
    runtime::ResetScheduler();
    runtime::InstallResume(&Resume, &host_);
  }

  // Whether two runs of the same program did the same thing.
  //
  // A trap is compared by its kind rather than by where it was reported: a
  // pass that turns one instruction into five -- which is what expanding
  // an element address does -- moves the trap onto a different one without
  // changing which element was out of bounds. What it must not do is trap
  // where the program would not have, or not trap where it would.
  static bool Agree(const interp::Outcome& left,
                    const interp::Outcome& right) {
    if (!left.ok() || !right.ok()) {
      // The text too: a trap reads the same in every tier (D226).
      return left.ok() == right.ok() && left.trap == right.trap &&
             left.message == right.message;
    }
    return Describe(left) == Describe(right);
  }

  static std::string Describe(const interp::Outcome& outcome) {
    if (outcome.ok()) {
      // A string is a pointer and a length in every tier, so comparing
      // one means comparing its bytes: the pointers differ between two
      // runs of the same program and say nothing about what it computed.
      if (const std::string text = AsText(outcome.value); !text.empty()) {
        return text;
      }
      return outcome.value.Describe();
    }
    return "trap: " + std::string(ir::TrapKindName(outcome.trap)) + " at " +
           outcome.where + ": " + outcome.message;
  }

  // The bytes of a value that looks like a string: a pair of a pointer
  // and a length. Empty when it is not one.
  static std::string AsText(const interp::Value& value) {
    if (value.kind() != interp::Value::Kind::kAggregate ||
        value.elements().size() != 2) {
      return {};
    }
    const interp::Value& data = value.elements()[0];
    const interp::Value& length = value.elements()[1];
    if (data.kind() != interp::Value::Kind::kInt ||
        length.kind() != interp::Value::Kind::kInt ||
        data.integer() == 0 || data.is_signed() || length.is_signed()) {
      return {};
    }
    const auto* bytes = reinterpret_cast<const char*>(data.integer());
    return "\"" + std::string(bytes, static_cast<size_t>(
                                          length.integer())) +
           "\"";
  }

  // A string argument: the pair, pointing at bytes the fixture keeps
  // alive for as long as the run.
  static interp::Value Text(std::string_view bytes) {
    static std::vector<std::unique_ptr<std::string>> kept;
    kept.push_back(std::make_unique<std::string>(bytes));
    const std::string& storage = *kept.back();
    std::vector<interp::Value> pair;
    pair.push_back(interp::Value::Int(
        reinterpret_cast<uint64_t>(storage.data()), IntWidth::k64, false));
    pair.push_back(
        interp::Value::Int(storage.size(), IntWidth::k64, false));
    return interp::Value::Aggregate(std::move(pair));
  }

  static interp::Value Int(int64_t value) {
    return interp::Value::Int(static_cast<uint64_t>(value), IntWidth::k64,
                              true);
  }
  static interp::Value S32(int64_t value) {
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
  std::unique_ptr<interp::ForeignCaller> foreign_;
  Host host_;
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
  const v = if c { 10 } else { 20 }
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

fun Overflow(a: s8, b: s8) -> s8 => a + b

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
                   "1099511627776");
  WOLANG_EXPECT_EQ(f.Run("Shift", {Fixture::Int(1), Fixture::Int(62)}),
                   "4611686018427387904");
  WOLANG_EXPECT_EQ(f.Run("Shift", {Fixture::Int(1), Fixture::Int(70)}),
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
                   "0.3333333333333333");
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
fun Widen(n: s32) -> s64 => n as s64

fun Narrow(n: s32) -> u8 => n as u8

fun ToFloat(n: int) -> float => n as float

fun Truncate(x: float) -> int => x as int
)");
  WOLANG_EXPECT_EQ(f.Run("Widen", {Fixture::S32(-5)}), "-5");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {Fixture::S32(200)}), "200");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {Fixture::S32(300)}),
                   "trap: overflow at @Narrow:%1: "
                   "the value does not fit the integer it was cast to");
  WOLANG_EXPECT_EQ(f.Run("ToFloat", {Fixture::Int(3)}), "3");
  WOLANG_EXPECT_EQ(f.Run("Truncate", {Fixture::Float(-2.75)}), "-2");
}

// Run fails the test itself when the tiers disagree (D226).
WOLANG_TEST(CodegenTest, TrapsOnSignChangingCastsAt64Bits) {
  Fixture f(R"(
fun ToUnsigned(n: s64) -> u64 => n as u64

fun ToSigned(n: u64) -> s64 => n as s64

fun Promote(n: s32) -> u64 => n as u64
)");
  const char* kTrap =
      "trap: overflow at @%s:%%1: "
      "the value does not fit the integer it was cast to";
  char text[160];
  std::snprintf(text, sizeof(text), kTrap, "ToUnsigned");
  WOLANG_EXPECT_EQ(f.Run("ToUnsigned", {Fixture::Wide(-1)}), text);
  WOLANG_EXPECT_EQ(f.Run("ToUnsigned", {Fixture::Wide(7)}), "7");
  std::snprintf(text, sizeof(text), kTrap, "ToSigned");
  WOLANG_EXPECT_EQ(
      f.Run("ToSigned",
            {interp::Value::Int(~uint64_t{0}, IntWidth::k64, false)}),
      text);
  WOLANG_EXPECT_EQ(
      f.Run("ToSigned", {interp::Value::Int(7, IntWidth::k64, false)}),
      "7");
  std::snprintf(text, sizeof(text), kTrap, "Promote");
  WOLANG_EXPECT_EQ(f.Run("Promote", {Fixture::S32(-1)}), text);
}

WOLANG_TEST(CodegenTest, ANegativeFloatCastToANarrowUnsignedTraps) {
  // Found by the fuzzer on arm64: `fcvtzu` clamps -2.5 to zero, which fits
  // any unsigned type, so the cast returned 0 where the interpreter traps.
  // Minus a half truncates to zero and is in range in every tier.
  Fixture f(R"(
fun ToByte(x: f64) -> int => (x as u8) as int

fun ToWord(x: f64) -> int => (x as u32) as int
)");
  const char* kTrap =
      "trap: overflow at @%s:%%1: "
      "the value does not fit the integer it was cast to";
  char text[160];
  std::snprintf(text, sizeof(text), kTrap, "ToByte");
  WOLANG_EXPECT_EQ(f.Run("ToByte", {Fixture::Double(-2.5)}), text);
  WOLANG_EXPECT_EQ(f.Run("ToByte", {Fixture::Double(-0.5)}), "0");
  WOLANG_EXPECT_EQ(f.Run("ToByte", {Fixture::Double(200.5)}), "200");
  std::snprintf(text, sizeof(text), kTrap, "ToWord");
  WOLANG_EXPECT_EQ(f.Run("ToWord", {Fixture::Double(-1.0)}), text);
}

WOLANG_TEST(CodegenTest, TrapTextIsTheSameInEveryTier) {
  // Run compares the text of a trap between the interpreter and the
  // machine code, so one case of each kind that used to differ is enough
  // (D226): an index into a slice, whose length is a runtime value, a
  // narrow negation, and a division.
  Fixture f(R"(
fun Pick(i: int) -> int {
  var buf: [3]int
  return Take(buf, i)
}

fun Take(data: []int, i: int) -> int => data[i]

fun Negate(n: s8) -> s8 => -n

fun Divide(a: int, b: int) -> int => a / b
)");
  WOLANG_EXPECT_EQ(f.Run("Pick", {Fixture::Int(1)}), "0");
  WOLANG_EXPECT_EQ(f.Run("Pick", {Fixture::Int(5)}),
                   "trap: out of bounds at @Pick:%15: "
                   "index is outside the length");
  WOLANG_EXPECT_EQ(f.Run("Negate", {Fixture::Byte(-128)}),
                   "trap: overflow at @Negate:%1: negation overflowed");
  WOLANG_EXPECT_EQ(f.Run("Divide", {Fixture::Wide(1), Fixture::Wide(0)}),
                   "trap: division by zero at @Divide:%2: divided by zero");
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
  const xs = [10, 20, 30]
  return xs[i]
}
)");
  WOLANG_EXPECT_EQ(f.Run("At", {Fixture::Int(1)}), "20");
  WOLANG_EXPECT_EQ(f.Run("At", {Fixture::Int(3)}),
                   "trap: out of bounds at @At:%6: "
                   "index is outside a length of 3");
  WOLANG_EXPECT_EQ(f.Run("At", {Fixture::Int(-1)}),
                   "trap: out of bounds at @At:%6: "
                   "index is outside a length of 3");
}

WOLANG_TEST(CodegenTest, RunsOptionals) {
  Fixture f(R"(
fun Unwrap(p: int?) -> int {
  if const v = p {
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

WOLANG_TEST(CodegenTest, CarriesASliceThroughTwoLoops) {
  // A slice travelling as a block parameter through two loops in a row.
  // The dev tier traded block parameters through a word-sized slot, so
  // the slice kept its address and lost its length -- and the second
  // loop compared against garbage (D107).
  Fixture f(R"(
fun Probe() -> int {
  var room = [0, 0, 0, 0]
  var items = room[0..4]
  var i = 0
  while i < items.len {
    items[i] = i
    i = i + 1
  }
  var total = 0
  i = 0
  while i < items.len {
    total = total + items[i]
    i = i + 1
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Probe"), "6");
}

WOLANG_TEST(CodegenTest, RunsAClosureThatCaptures) {
  // A closure takes a *copy* of what it reads, in an environment that
  // lives as long as the call that made it. Its value is the pair every
  // function value is -- the code and the environment -- and which of
  // the two shapes to call is a question about the value (D138).
  Fixture f(R"(
fun Apply(f: fun(int) -> int, x: int) -> int => f(x)

fun Fold(f: fun(int, int) -> int, xs: [4]int) -> int {
  var total = 0
  var i = 0
  while i < 4 {
    total = f(total, xs[i])
    i = i + 1
  }
  return total
}

fun Twice(n: int) -> int => n * 2

fun Probe() -> int {
  const factor = 3
  const offset = 4
  var xs: [4]int
  xs[0] = 1
  xs[1] = 2
  xs[2] = 3
  xs[3] = 4
  // One that captures two names, one that captures nothing, a plain
  // function through the same parameter, and one capturing inside a
  // loop body.
  const scaled = Apply(|n| n * factor + offset, 10)
  const plain = Apply(|n| n + 1, 10)
  const named = Apply(Twice, 10)
  const folded = Fold(|a, b| a + b + factor, xs)
  return scaled + plain + named + folded
}
)");
  // 34 + 11 + 20 + (10 + 12) = 87.
  WOLANG_EXPECT_EQ(f.Run("Probe"), "87");
}

WOLANG_TEST(CodegenTest, GivesAnInterpolatedStringBack) {
  // An interpolated string is allocated, so somebody has to free it: the
  // binding that holds it, when its scope ends. The pieces a
  // concatenation consumed go back immediately -- a string built from
  // three parts is one live allocation, not three (D133).
  //
  // What this test can check is that the program still answers: that the
  // *memory* goes back is what `20000000` iterations in a flat
  // twenty-six megabytes showed.
  Fixture f(R"(
fun Build(n: int) -> int {
  const message = "value {n} of many"
  return message.len
}

fun Probe() -> int {
  var total = 0
  var i = 0
  while i < 100 {
    total = total + Build(i)
    i = i + 1
  }
  return total
}
)");
  // Fifteen bytes of fixed text, plus one or two digits per value.
  WOLANG_EXPECT_EQ(f.Run("Probe"), "1590");
}

WOLANG_TEST(CodegenTest, ReachesAFarOffsetInALargeFrame) {
  // A frame with a four-kilobyte buffer in it puts the locals after it
  // out of reach of both addressing forms, and the assembler used to
  // insist one of them fit. The address is computed instead (D124).
  Fixture f(R"(
fun Probe() -> int {
  var buffer: [4096]u8
  var total = 0
  buffer[0] = 7
  buffer[4095] = 9
  total = (buffer[0] as int) + (buffer[4095] as int)
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Probe"), "16");
}

WOLANG_TEST(CodegenTest, DoesNotLetAMethodShadowAFunction) {
  // A method lives in the module's function table under its owner's name
  // *and* was findable by its bare name, so a plain call to a function
  // called the same thing went to the method -- with no receiver, which
  // the verifier caught and nothing else did (D123).
  Fixture f(R"(
fun Twice(n: int) -> int => n * 2

class Box {
  var value: int = 3

  fun Twice(n: int) -> int => n * 10 + value
}

fun Probe() -> int {
  const box = Box(3)
  return Twice(4) + box.Twice(4)
}
)");
  // Eight from the function, forty-three from the method.
  WOLANG_EXPECT_EQ(f.Run("Probe"), "51");
}

WOLANG_TEST(CodegenTest, KeepsATagOnlyEnumThroughAGlobal) {
  // The interpreter holds an enum with no payload as a one-field
  // aggregate, and memory holds it as a number: a global one had to be
  // read back in the shape `match` reads, not as a bare number (D120).
  Fixture f(R"(
enum Colour {
  Red,
  Green,
  Blue,
}

var chosen: Colour = Colour.Green

fun Rank(colour: Colour) -> int {
  return match colour {
    Colour.Red => 0,
    Colour.Green => 1,
    Colour.Blue => 2,
  }
}

fun Probe() -> int {
  const before = Rank(chosen)
  chosen = Colour.Blue
  return before * 10 + Rank(chosen)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Probe"), "12");
}

WOLANG_TEST(CodegenTest, MatchReadsOnlyTheTag) {
  // A tag is u32 wherever it is read (D238): `match` must not read the
  // word beside it, which here is an s32 of -1 and a payload.
  Fixture f(R"(
enum Colour {
  Red,
  Green,
  Blue,
}

enum Shape {
  Dot,
  Box(h: u64),
}

struct Paint {
  var colour: Colour
  var after: s32
}

fun Rank(colour: Colour) -> int {
  return match colour {
    Colour.Red => 0,
    Colour.Green => 1,
    Colour.Blue => 2,
  }
}

fun Size(shape: Shape) -> int {
  return match shape {
    Shape.Dot => 0,
    Shape.Box(h) => h as int,
  }
}

fun Probe() -> int {
  var paint = Paint(Colour.Blue, -1)
  var shapes: [2]Shape
  shapes[0] = Shape.Box(7)
  shapes[1] = Shape.Dot
  return Rank(paint.colour) * 100 + Size(shapes[0]) * 10 + Size(shapes[1]) + paint.after as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("Probe"), "269");
}

WOLANG_TEST(CodegenTest, RunsAGenericMethod) {
  // A method with type parameters of its own, on a type that has none:
  // one copy per set of arguments, and the closure's parameter types come
  // from the half of the signature inference already knows (D119).
  Fixture f(R"(
class Box {
  var value: int

  fun Map<T>(f: fun(int) -> T) -> T {
    return f(value)
  }
}

fun Half(n: int) -> f32 => (n as f32) / 2.0

fun Probe() -> int {
  const box = Box(7)
  const doubled = box.Map(|n| n * 2)
  const halved = box.Map(Half)
  return doubled + (halved as int)
}
)");
  // Fourteen and three: one copy of `Map` per result type.
  WOLANG_EXPECT_EQ(f.Run("Probe"), "17");
}

WOLANG_TEST(CodegenTest, RunsClosures) {
  // A closure is a function the program wrote where it is used, and its
  // value is that function's address: the parameter types come from the
  // type it is being given to (D106).
  Fixture f(R"(
fun Apply(f: fun(int) -> int, x: int) -> int {
  return f(x)
}

fun Fold(f: fun(int, int) -> int, xs: [4]int) -> int {
  var total = 0
  var i = 0
  while i < 4 {
    total = f(total, xs[i])
    i = i + 1
  }
  return total
}

fun Probe() -> int {
  const inferred = Apply(|n| n + 1, 10)
  const written = Apply(|n: int| -> int { return n * 3 }, 4)
  const folded = Fold(|a, b| a + b, [1, 2, 3, 4])
  const nested = Apply(|n| Apply(|m| m * 2, n) + 1, 5)
  return inferred + written + folded + nested
}
)");
  // Eleven, twelve, ten and eleven.
  WOLANG_EXPECT_EQ(f.Run("Probe"), "44");
}

WOLANG_TEST(CodegenTest, SaysWhatItCannotCompile) {
  // A tier that compiles what it can has to say what it did not, or "why
  // is this slow" has no answer.
  Fixture f(R"(
class Held { var value: int = 0 }

fun Many(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int,
         i: int, j: int) -> int => a + j

fun Keeps(held: Held) -> int => held.value

fun Count(n: int) -> int => n + 1
)");
  // Ten arguments is no longer one of the reasons: what does not fit the
  // registers goes on the stack (D154). A class is -- it needs the
  // collector, which this tier does not emit.
  WOLANG_EXPECT_EQ(f.Skipped("Many"), "");
  WOLANG_EXPECT_EQ(f.Skipped("Count"), "");
}

WOLANG_TEST(CodegenTest, SpillsWhenThereAreMoreValuesThanRegisters) {
  // Nine integer registers, and this holds more than nine values at once.
  Fixture f(R"(
fun Many(a: int, b: int, c: int, d: int, e: int) -> int {
  const p = a + b
  const q = b + c
  const r = c + d
  const s = d + e
  const t = e + a
  const u = p + q
  const v = q + r
  const w = r + s
  const x = s + t
  const y = t + u
  const z = u + v
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
  WOLANG_EXPECT_EQ(f.Skipped("Larger<s64>"), "");
  WOLANG_EXPECT_EQ(f.Skipped("Larger<f32>"), "");
}

WOLANG_TEST(CodegenTest, CallsOutOfTheLanguage) {
  // Both tiers reach C: compiled code calls the address the loader gave,
  // and the interpreter calls through a thunk the code generator made. If
  // only one of them could, every program using the FFI would fall out of
  // the differential check.
  Fixture f(R"(
extern "C" fun abs(n: s32) -> s32

extern "C" fun fabs(x: f64) -> f64

fun Magnitude(n: int) -> int => abs(n as s32) as int

fun Distance(x: f64) -> f64 => fabs(x)
)");
  WOLANG_EXPECT_EQ(f.Run("Magnitude", {Fixture::Int(-7)}), "7");
  WOLANG_EXPECT_EQ(f.Run("Magnitude", {Fixture::Int(7)}), "7");
  WOLANG_EXPECT_EQ(f.Run("Distance", {Fixture::Double(-2.5)}), "2.5");
}

WOLANG_TEST(CodegenTest, SaysWhenAForeignSymbolIsMissing) {
  Fixture f(R"(
extern "C" fun NotInAnyLibrary(n: s32) -> s32

fun Try(n: int) -> int => NotInAnyLibrary(n as s32) as int
)");
  WOLANG_EXPECT_EQ(f.Skipped("Try"),
                   "calls something this tier cannot compile");
  WOLANG_EXPECT_EQ(f.Skipped("NotInAnyLibrary"),
                   "is `extern` and its symbol was not found");
}

WOLANG_TEST(CodegenTest, AllocatesThroughTheForeignBoundary) {
  // The heap, before there is a language feature for it: `malloc` is a
  // foreign function, a `*i32` is an address, and reading through one is
  // the same load as reading a local. Both tiers do it, which is what makes
  // this comparable at all.
  Fixture f(R"(
extern "C" fun malloc(size: u64) -> *s32
extern "C" fun free(block: *s32)

fun RoundTrip(n: int) -> int {
  const block = malloc(4)
  *block = (n + 1) as s32
  const read = *block
  free(block)
  return read as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("RoundTrip", {Fixture::Int(41)}), "42");
  WOLANG_EXPECT_EQ(f.Run("RoundTrip", {Fixture::Int(-1)}), "0");
}

WOLANG_TEST(CodegenTest, TakesTheAddressOfALocal) {
  Fixture f(R"(
fun Through(n: int) -> int {
  var x = n
  const p = &x
  *p = *p + 1
  return x
}
)");
  WOLANG_EXPECT_EQ(f.Run("Through", {Fixture::Int(5)}), "6");
}

WOLANG_TEST(CodegenTest, CarriesABorrowedString) {
  // A `str` is a pointer and a length, and a literal is a view of bytes
  // that live with the module -- which is why it is a borrow and why it
  // costs no allocation.
  Fixture f(R"(
fun Which(c: bool) -> text {
  if c {
    return "yes"
  }
  return "no"
}

fun Length(message: text) -> int => 0
)");
  WOLANG_EXPECT_EQ(f.Run("Which", {Fixture::Bool(true)}), "\"yes\"");
  WOLANG_EXPECT_EQ(f.Run("Which", {Fixture::Bool(false)}), "\"no\"");
  WOLANG_EXPECT_EQ(f.Skipped("Length"), "");
}

WOLANG_TEST(CodegenTest, RunsDeferredStatementsOnEveryExit) {
  // A `defer` is the one statement whose position in the source is not
  // where it runs. Lowering is where that stops being true: it becomes
  // ordinary code at every exit from the scope that deferred it.
  Fixture f(R"(
fun Order() -> int {
  var total = 0
  {
    defer total = total * 2
    defer total = total + 3
    total = 1
  }
  return total
}

fun EarlyOut(c: bool) -> int {
  var seen = 0
  {
    defer seen = seen + 10
    if c {
      return seen + 1
    }
  }
  return seen
}

fun PerIteration(n: int) -> int {
  var total = 0
  var i = 0
  while i < n {
    defer total = total + 1
    i = i + 1
  }
  return total
}
)");
  // Innermost first, so `+ 3` runs before `* 2`.
  WOLANG_EXPECT_EQ(f.Run("Order"), "8");
  // The returned value is computed before the deferred statements run, so a
  // `defer` cannot change the answer -- only what is left behind.
  WOLANG_EXPECT_EQ(f.Run("EarlyOut", {Fixture::Bool(true)}), "1");
  WOLANG_EXPECT_EQ(f.Run("EarlyOut", {Fixture::Bool(false)}), "10");
  WOLANG_EXPECT_EQ(f.Run("PerIteration", {Fixture::Int(5)}), "5");
}

WOLANG_TEST(CodegenTest, DeferIsWhatMakesTheHeapUsable) {
  // Until there is an owning type, this is memory management: allocate,
  // defer the release, and the release happens on every path out.
  Fixture f(R"(
extern "C" fun malloc(size: u64) -> *s32
extern "C" fun free(block: *s32)

fun Scoped(n: int) -> int {
  const block = malloc(4)
  defer free(block)
  *block = (n + 1) as s32
  return *block as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("Scoped", {Fixture::Int(41)}), "42");
}

WOLANG_TEST(CodegenTest, RunsAStacklessCoroutine) {
  // A `task` is split into a state machine before anything runs, so both
  // tiers execute ordinary functions and structs -- which is why a
  // coroutine is covered by the same differential check as everything else.
  Fixture f(R"(
task fun Yield()

task fun Twice(n: int) -> int {
  wait Yield()
  const doubled = n * 2
  wait Yield()
  return doubled + n
}

task fun Outer(n: int) -> int {
  const once = wait Twice(n)
  const twice = wait Twice(once)
  return twice
}
)");
  // Two suspensions and a result that survives both of them.
  WOLANG_EXPECT_EQ(f.Run("Twice", {Fixture::Int(5)}), "15");
  // A task waiting on a task: the callee's frame lives inside the caller's.
  WOLANG_EXPECT_EQ(f.Run("Outer", {Fixture::Int(2)}), "18");
}

WOLANG_TEST(CodegenTest, SuspendsInsideALoop) {
  // The state the machine comes back to is a block in the middle of a loop,
  // which is the case a state machine has to get right and the one a
  // dominance check catches when it does not.
  Fixture f(R"(
task fun Yield()

task fun Count(n: int) -> int {
  var total = 0
  var i = 0
  while i < n {
    wait Yield()
    total = total + i
    i = i + 1
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Count", {Fixture::Int(5)}), "10");
  WOLANG_EXPECT_EQ(f.Run("Count", {Fixture::Int(0)}), "0");
  WOLANG_EXPECT_EQ(f.Run("Count", {Fixture::Int(100)}), "4950");
}

WOLANG_TEST(CodegenTest, SpawnsTasksAndRunsThemToCompletion) {
  // `spawn` allocates a frame, writes the arguments into it and hands the
  // pair to the scheduler; the scheduler advances every task one step at a
  // time through the one entry point the compiler generates. Which is why
  // this works the same interpreted and compiled.
  Fixture f(R"(
extern "C" fun wolang_sched_run() -> s64
extern "C" fun wolang_sched_pending() -> s32

task fun Yield()

task fun Rounds(n: int) -> int {
  var done = 0
  while done < n {
    wait Yield()
    done = done + 1
  }
  return done
}

fun Many(count: int) -> s64 uses unsafe {
  var started = 0
  while started < count {
    const _handle = spawn Rounds(2)
    started = started + 1
  }
  return wolang_sched_run()
}

fun LeftOver(count: int) -> int uses unsafe {
  var started = 0
  while started < count {
    const _handle = spawn Rounds(1)
    started = started + 1
  }
  const _steps = wolang_sched_run()
  return wolang_sched_pending() as int
}
)");
  // Three steps each: two suspensions and the one that finishes.
  WOLANG_EXPECT_EQ(f.Run("Many", {Fixture::Int(4)}), "12");
  WOLANG_EXPECT_EQ(f.Run("Many", {Fixture::Int(0)}), "0");
  // Nothing is left waiting once the queue drains.
  WOLANG_EXPECT_EQ(f.Run("LeftOver", {Fixture::Int(10)}), "0");
}

WOLANG_TEST(CodegenTest, CallsMethodsAndWritesFields) {
  // A method is a function whose first parameter is the receiver, and a
  // field of a place is read and written where it is -- which before this
  // wave was an undefined value and an assignment that compiled to
  // nothing.
  Fixture f(R"(
struct Inner {
  var n: int = 0
}

struct Outer {
  var inner: Inner = Inner(0)
  var count: int = 0
}

struct Point {
  var x: int = 0
  var y: int = 0

  fun Sum() -> int => x + y
  fun Shifted(by: int) -> Point => Point(x + by, y + by)
  // A method calling another method of its own type, with the receiver
  // left implicit.
  fun Twice() -> int => Sum() + Sum()
}

fun Reads() -> int {
  const p = Point(3, 4)
  return p.Sum()
}

fun Chains() -> int {
  const p = Point(1, 2)
  return p.Shifted(10).Shifted(100).Sum()
}

fun Implicit() -> int {
  return Point(5, 6).Twice()
}

fun WritesAField() -> int {
  var p = Point(1, 2)
  p.x = 100
  p.y = p.y + 1
  return p.Sum()
}

fun WritesANestedField() -> int {
  var o = Outer(Inner(1), 2)
  o.inner.n = 40
  o.count = o.count + 1
  return o.inner.n + o.count
}
)");
  WOLANG_EXPECT_EQ(f.Run("Reads", {}), "7");
  // (1, 2) shifted by ten is (11, 12), then by a hundred is
  // (111, 112), which sums to 223.
  WOLANG_EXPECT_EQ(f.Run("Chains", {}), "223");
  WOLANG_EXPECT_EQ(f.Run("Implicit", {}), "22");
  WOLANG_EXPECT_EQ(f.Run("WritesAField", {}), "103");
  WOLANG_EXPECT_EQ(f.Run("WritesANestedField", {}), "43");
}

WOLANG_TEST(CodegenTest, AClassIsAHandleToAHeapObject) {
  // Which is the whole difference from a struct: the receiver of a method
  // is the address of the object, so a method that assigns to a field
  // changes the object it was called on. The handle being `const` freezes
  // the handle and not what it points at.
  Fixture f(R"(
class Counter {
  var value: int = 0
  var step: int = 1

  fun Bump() {
    value = value + step
  }

  fun Value() -> int => value
}

fun Mutates() -> int {
  const c = Counter(10, 5)
  c.Bump()
  c.Bump()
  return c.Value()
}

fun WritesThroughTheHandle() -> int {
  const c = Counter(0, 1)
  c.step = 7
  c.Bump()
  return c.value
}

fun TwoObjectsAreTwoObjects() -> int {
  const a = Counter(0, 1)
  const b = Counter(0, 10)
  a.Bump()
  b.Bump()
  return a.value * 100 + b.value
}

// A handle passed to a function is the same object, not a copy of it.
fun BumpTwice(c: Counter) {
  c.Bump()
  c.Bump()
}

fun SharesTheObject() -> int {
  const c = Counter(0, 3)
  BumpTwice(c)
  return c.value
}
)");
  WOLANG_EXPECT_EQ(f.Run("Mutates", {}), "20");
  WOLANG_EXPECT_EQ(f.Run("WritesThroughTheHandle", {}), "7");
  WOLANG_EXPECT_EQ(f.Run("TwoObjectsAreTwoObjects", {}), "110");
  WOLANG_EXPECT_EQ(f.Run("SharesTheObject", {}), "6");
}

WOLANG_TEST(CodegenTest, CallsThroughAFunctionValue) {
  // A function used as a value: a comparator, a route's handler, a
  // signal's subscriber. Compiled code can only reach compiled code, so
  // the address is one this tier produced -- and because it is an absolute
  // address rather than a displacement, the block is placed before the
  // code is finished.
  Fixture f(R"(
fun Double(n: int) -> int => n * 2
fun Negate(n: int) -> int => 0 - n

fun Apply(f: fun(int) -> int, n: int) -> int {
  return f(n)
}

fun Twice(f: fun(int) -> int, n: int) -> int {
  return f(f(n))
}

fun Pick(which: bool) -> fun(int) -> int {
  if which {
    return Double
  }
  return Negate
}

fun Direct() -> int {
  return Apply(Double, 21)
}

fun Nested() -> int {
  return Twice(Double, 3)
}

fun Returned(which: bool) -> int {
  const chosen = Pick(which)
  return chosen(5)
}

fun StoredInASlot() -> int {
  var f = Double
  var total = f(1)
  f = Negate
  total = total + f(10)
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Direct", {}), "42");
  WOLANG_EXPECT_EQ(f.Run("Nested", {}), "12");
  WOLANG_EXPECT_EQ(f.Run("Returned", {Fixture::Bool(true)}), "10");
  WOLANG_EXPECT_EQ(f.Run("Returned", {Fixture::Bool(false)}), "-5");
  WOLANG_EXPECT_EQ(f.Run("StoredInASlot", {}), "-8");
}

WOLANG_TEST(CodegenTest, CountsAndFillsBuffers) {
  // `for` over a range and over a sequence, and writing one element of a
  // buffer. All three were silently nothing before this wave: the loop
  // lowered to an undefined value and the element write to no instruction
  // at all, so a program that filled a buffer computed with zeroes.
  Fixture f(R"(
fun Count(n: int) -> int {
  var total = 0
  for i in 0..n {
    total = total + i
  }
  return total
}

fun Inclusive(n: int) -> int {
  var total = 0
  for i in 1..=n {
    total = total + i
  }
  return total
}

fun FillAndSum() -> int {
  var buf: [8]int
  for i in 0..8 {
    buf[i] = i * i
  }
  var total = 0
  for v in buf {
    total = total + v
  }
  return total
}

fun WritesOneElement() -> int {
  var buf: [4]int
  buf[2] = 7
  // The rest is zero: a slot starts that way in every tier, which is what
  // makes filling a buffer one element at a time work.
  return buf[0] + buf[1] + buf[2] + buf[3]
}

fun BreaksAndContinues(n: int) -> int {
  var total = 0
  for i in 0..n {
    if i == 5 {
      break
    }
    if i == 2 {
      continue
    }
    total = total + i
  }
  return total
}

fun NestedLoops(n: int) -> int {
  var total = 0
  for i in 0..n {
    for j in 0..n {
      total = total + i * j
    }
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Count", {Fixture::Int(10)}), "45");
  WOLANG_EXPECT_EQ(f.Run("Count", {Fixture::Int(0)}), "0");
  WOLANG_EXPECT_EQ(f.Run("Inclusive", {Fixture::Int(4)}), "10");
  WOLANG_EXPECT_EQ(f.Run("FillAndSum", {}), "140");
  WOLANG_EXPECT_EQ(f.Run("WritesOneElement", {}), "7");
  WOLANG_EXPECT_EQ(f.Run("BreaksAndContinues", {Fixture::Int(100)}), "8");
  WOLANG_EXPECT_EQ(f.Run("NestedLoops", {Fixture::Int(4)}), "36");
}

WOLANG_TEST(CodegenTest, PassesArraysAsSlices) {
  // A slice is a pointer and a length, so an array converts to one and a
  // function that takes `[]T` reads the caller's elements rather than a
  // copy of them. Which is also why the bounds check has to come from the
  // value: the length of a slice is not something its type knows.
  Fixture f(R"(
fun Sum(data: []int) -> int {
  var total = 0
  for v in data {
    total = total + v
  }
  return total
}

fun First(data: []int) -> int {
  return data[0]
}

fun Length(data: []int) -> int {
  return data.len
}

fun Fill() -> int {
  var buf: [4]int
  for i in 0..4 {
    buf[i] = i + 1
  }
  return Sum(buf) + First(buf) + Length(buf) + buf.len
}

fun ReadsThroughTheSlice() -> int {
  var buf: [3]int
  buf[0] = 7
  buf[1] = 8
  buf[2] = 9
  return Sum(buf) - First(buf)
}
)");
  // 1 + 2 + 3 + 4, then the first element, then the length twice.
  WOLANG_EXPECT_EQ(f.Run("Fill", {}), "19");
  WOLANG_EXPECT_EQ(f.Run("ReadsThroughTheSlice", {}), "17");
}

WOLANG_TEST(CodegenTest, EvaluatesAnIndexOnce) {
  // Asking where a value lives and then reading it are one question, not
  // two: when they were two, the index expression was lowered twice. The
  // dead copy cost a quarter of the array benchmark, and a side effect in
  // an index would have happened twice -- which is the part a benchmark
  // would never have told us.
  Fixture f(R"(
class Counter {
  var n: int = 0

  fun Next() -> int {
    n = n + 1
    return n
  }
}

fun Once() -> int {
  const data = [10, 20, 30, 40]
  const c = Counter(0)
  const value = data[c.Next()]
  // The index ran once, so the counter is one and the element is the
  // second.
  return value * 10 + c.n
}

fun OnceIntoABuffer() -> int {
  var buf: [4]int
  const c = Counter(0)
  buf[c.Next()] = 7
  return buf[1] * 10 + c.n
}
)");
  WOLANG_EXPECT_EQ(f.Run("Once", {}), "201");
  WOLANG_EXPECT_EQ(f.Run("OnceIntoABuffer", {}), "71");
}

WOLANG_TEST(CodegenTest, LaysOutARecordAsOneArrayPerField) {
  // `soa []T` is a layout, not a type: the same fields, written and read
  // the same way, with the storage turned inside out underneath. Which
  // means every tier has to agree about where a field of an element is --
  // and the reference tier has no addresses at all, so it computes the
  // same offsets by arithmetic on a block it allocated.
  //
  // The offsets are what this is really checking. A field's array starts
  // at the sizes of the fields before it times the padded count, so
  // writing one element's `life` must not land on another element's `x`:
  // the first version of this got 36 where it should have had 10 because
  // every field shared one array.
  Fixture f(R"(
offer fun SoaNew<T>(count: int) -> soa []T uses unsafe

struct Particle {
  var x: float = 0.0
  var y: float = 0.0
  var life: float = 0.0
}

fun Spread() -> int uses unsafe {
  const ps = SoaNew<Particle>(4)
  var i = 0
  while i < 4 {
    ps[i].x = (i + 1) as float
    ps[i].life = ((i + 1) * 10) as float
    i = i + 1
  }
  var total = 0.0
  var j = 0
  while j < 4 {
    total = total + ps[j].x + ps[j].life
    j = j + 1
  }
  // 1+2+3+4 and 10+20+30+40: nothing overlapped.
  return total as int
}

fun ReadsPastTheEnd() -> float uses unsafe {
  const ps = SoaNew<Particle>(2)
  return ps[2].y
}
)");
  WOLANG_EXPECT_EQ(f.Run("Spread", {}), "110");
  WOLANG_EXPECT(f.Run("ReadsPastTheEnd", {}).find("trap: out of bounds") ==
                size_t{0});
}

WOLANG_TEST(CodegenTest, TakesTheTypeArgumentACallWrites) {
  // `SoaNew<Particle>(4)` has nothing to infer from: the only parameter is
  // a count. Before the turbofish was read, the call's type stayed `soa
  // []T` and the field offsets were computed against a type parameter,
  // which laid every field of every element on top of the others.
  Fixture f(R"(
offer fun Empty<T>() -> []T uses unsafe

struct Pair {
  var a: int = 0
  var b: int = 0
}

fun Length() -> int uses unsafe {
  const xs = Empty<Pair>()
  return xs.len
}
)");
  // The declaration has no body, so this tier cannot run it -- what is
  // being checked is that the call is typed by what it wrote, which the
  // checker test below reads directly. Here it is enough that the whole
  // pipeline accepts it rather than lowering an address against `T`.
  WOLANG_EXPECT(f.Run("Length", {}).find("<verifier") == std::string::npos);
}

WOLANG_TEST(CodegenTest, BuildsAGenericTypeAndCallsItsMethods) {
  // A generic type is a recipe: `Box<int>` and `Box<float>` are two
  // layouts and two copies of every method, instantiated where they are
  // asked for the way a generic function is. Without that, a field of a
  // `Box<int>` was laid out against `T` -- so both fields of a two-field
  // instance landed at offset zero and reading one gave the other.
  Fixture f(R"(
struct Box<T> {
  var item: T
  var tag: int = 0

  fun Item() -> T {
    return item
  }
}

class Counter<T> {
  var item: T
  var count: int = 0

  fun Add() -> int {
    count = count + 1
    return count
  }
}

fun Written() -> int {
  const b = Box<int>(7, 3)
  return b.Item() * 10 + b.tag
}

fun Inferred() -> float {
  // No turbofish: the field a value is given to says what `T` is.
  const b = Box(2.5, 1)
  return b.Item()
}

fun ThroughAHandle() -> int {
  // A class is a handle, and an instance of one is too: the method
  // changes the object it was called on rather than a copy.
  var c = Counter<int>(5, 0)
  const first = c.Add()
  const second = c.Add()
  return first * 100 + second * 10 + c.item
}
)");
  WOLANG_EXPECT_EQ(f.Run("Written", {}), "73");
  WOLANG_EXPECT_EQ(f.Run("Inferred", {}), "2.5");
  WOLANG_EXPECT_EQ(f.Run("ThroughAHandle", {}), "125");
}

WOLANG_TEST(CodegenTest, ReadsAndWritesAnAggregateThroughAPointer) {
  // A class with a slice in it: the field is sixteen bytes rather than a
  // register's worth, so writing it means writing a pointer and a length
  // at the offsets the layout gives. Every tier has to agree, and the
  // reference tier had no way to do it at all -- which is what a store of
  // a whole aggregate through a pointer was, until an ECS store needed
  // one.
  // The storage is allocated rather than a local array: a local lives in
  // a frame slot, and the reference tier holds a slot as a value rather
  // than at an address, so there would be no pointer to write.
  Fixture f(R"(
offer fun ArrayNew<T>(count: int) -> []T uses unsafe

class Rows {
  var data: []int
  var count: int = 0
}

fun Through() -> int uses unsafe {
  const buf = ArrayNew<int>(4)
  buf[0] = 10
  buf[1] = 20
  buf[2] = 30
  var rows = Rows(buf, 0)
  // Read the whole slice back out of the object, which is the load side.
  const view = rows.data
  rows.count = view.len
  return view[0] + view[1] + view[2] + rows.count
}
)");
  WOLANG_EXPECT_EQ(f.Run("Through", {}), "64");
}

WOLANG_TEST(CodegenTest, AGenericStoreOverSoaStorage) {
  // The shape the ECS library is built out of: a generic class holding
  // `soa []T` and two plain arrays, with methods that read and write
  // them. Every piece of this works on its own, and putting them
  // together is what found the rest.
  Fixture f(R"(
offer fun SoaNew<T>(count: int) -> soa []T uses unsafe
offer fun ArrayNew<T>(count: int) -> []T uses unsafe

struct Point {
  var x: float = 0.0
  var y: float = 0.0
}

class Store<T> {
  var rows: soa []T
  var owners: []int
  var count: int = 0

  fun Add(e: int) -> int {
    if count >= owners.len {
      return -1
    }
    const row = count
    owners[row] = e
    count = count + 1
    return row
  }
}

// Built by a generic function of its own, which is how a library hands
// one out: the caller never writes the field list.
fun StoreNew<T>(capacity: int) -> Store<T> uses unsafe {
  return Store<T>(SoaNew<T>(capacity), ArrayNew<int>(capacity), 0)
}

fun Build() -> int uses unsafe {
  var store = StoreNew<Point>(4)
  const first = store.Add(10)
  const second = store.Add(20)
  store.rows[first].x = 1.0
  store.rows[second].x = 2.0
  return first + second * 10 + store.count * 100 +
         store.owners[1] * 1000 + (store.rows[second].x as int) * 100000
}
)");
  WOLANG_EXPECT_EQ(f.Run("Build", {}), "220210");
}

WOLANG_TEST(CodegenTest, WritesThroughASliceFieldOfAnObject) {
  Fixture f(R"(
offer fun ArrayNew<T>(count: int) -> []T uses unsafe

class Store {
  var owners: []int
  var count: int = 0
}

fun Body() -> bool uses unsafe {
  var store = Store(ArrayNew<int>(4), 0)
  store.owners[0] = 7
  return store.owners[0] == 7 && store.owners.len == 4
}
)");
  WOLANG_EXPECT_EQ(f.Run("Body", {}), "true");
}

WOLANG_TEST(CodegenTest, DispatchesAVirtualMethodThroughTheObject) {
  // What `virtual` is for: the body that runs is the one the object's
  // class has, not the one the handle's type has. Three levels, because
  // two would not show a slot being inherited past the class that
  // overrode it -- `Cube` overrides `Area` again and leaves `Name` to
  // `Shape`.
  //
  // The number in the object is the class's declaration, which is what
  // lets both tiers agree about it (D85): an object made by compiled code
  // dispatches in the interpreter and the other way round.
  Fixture f(R"(
class Shape {
  var tag: int = 0

  virtual fun Area() -> int {
    return 1
  }

  virtual fun Name() -> int {
    return 10
  }
}

class Square : Shape {
  var side: int = 2

  override fun Area() -> int {
    return side * side
  }
}

class Cube : Square {
  override fun Area() -> int {
    return side * side * side
  }

  override fun Name() -> int {
    return 30
  }
}

fun Sum(s: Shape) -> int {
  return s.Area() * 100 + s.Name()
}

fun Dispatch() -> int {
  var shape = Shape(0)
  var square = Square(0, 3)
  var cube = Cube(0, 2)
  // 110 + 910 + 830: the base's own, the square's area with the base's
  // name, and the cube's both.
  return Sum(shape) + Sum(square) + Sum(cube)
}

fun Inherited() -> int {
  // A field declared by a base, read by a derived class's method, and the
  // constructor taking both in layout order.
  var cube = Cube(7, 4)
  return cube.tag * 1000 + cube.side * 10 + cube.Name()
}
)");
  WOLANG_EXPECT_EQ(f.Run("Dispatch", {}), "1850");
  WOLANG_EXPECT_EQ(f.Run("Inherited", {}), "7070");
}

WOLANG_TEST(CodegenTest, CallsAMethodThroughAnAbilityBound) {
  // Static dispatch: the bound says every `T` has an `Area`, so the body
  // calls it and one copy of the function is made per type that was
  // passed. Which is what makes a generic algorithm over an ability cost
  // nothing at run time.
  Fixture f(R"(
ability Shape {
  fun Area() -> int
}

struct Circle {
  var r: int = 1
}

struct Square {
  var side: int = 1
}

extend Circle as Shape {
  fun Area() -> int {
    return r * r * 3
  }
}

extend Square as Shape {
  fun Area() -> int {
    return side * side
  }
}

fun Total<T: Shape>(s: T) -> int {
  return s.Area()
}

fun Both() -> int {
  const c = Circle(2)
  const q = Square(3)
  return Total(c) * 100 + Total(q)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Both", {}), "1209");
}

WOLANG_TEST(CodegenTest, CallsThroughAnAbilityValue) {
  // Dynamic dispatch without inheritance: an ability value is a pair -- what
  // it points at and which class that is -- and the call finds the body
  // from the pair alone (D86). A class hands over its handle; a struct is
  // copied onto the heap, because an ability value outlives the expression
  // that made it.
  //
  // The two implementations have nothing in common, which is the point:
  // one is a value type and the other a handle, and `Measure` was
  // compiled knowing neither.
  Fixture f(R"(
ability Shape {
  fun Area() -> int
}

struct Circle {
  var r: int = 1
}

class Barrel {
  var hp: int = 3
}

extend Circle as Shape {
  fun Area() -> int {
    return r * r * 3
  }
}

extend Barrel as Shape {
  fun Area() -> int {
    return hp * 100
  }
}

fun Measure(s: Shape) -> int {
  return s.Area()
}

fun Both() -> int {
  const c = Circle(2)
  var b = Barrel(4)
  return Measure(c) * 1000 + Measure(b)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Both", {}), "12400");
}

WOLANG_TEST(CodegenTest, RunsADestructorWhenAValueGoesAway) {
  // `deinit` is what a type does when a value of it goes away, and a
  // module-level `var` is how the test sees that it happened (D87, D88).
  //
  // The second half is the part that needs the analysis: handing the
  // value to something else *moves* it, so the scope that gave it away
  // must not destroy it and the one that took it must.
  Fixture f(R"(
var destroyed: int = 0

class Buffer {
  var size: int = 0

  deinit {
    destroyed = destroyed + 1
  }
}

fun Take(b: Buffer) -> int {
  // Borrowed: a class is counted, not moved (D209), so the caller still
  // holds it.
  return b.size
}

fun Counted() -> int {
  var round = 0
  while round < 3 {
    var b = Buffer(1)
    // Read through the handle, which is a borrow and not a move: the
    // loop body still owns it at the end of the iteration.
    round = round + b.size
  }
  return destroyed
}

fun Moved() -> int {
  var b = Buffer(7)
  const size = Take(b)
  // None yet: `b` is still held here, and goes when this scope ends.
  return size * 100 + destroyed
}

fun Gone() -> int {
  destroyed = 0
  const _size = Moved()
  return destroyed
}
)");
  WOLANG_EXPECT_EQ(f.Run("Counted", {}), "3");
  WOLANG_EXPECT_EQ(f.Run("Moved", {}), "700");
  WOLANG_EXPECT_EQ(f.Run("Gone", {}), "1");
}

WOLANG_TEST(CodegenTest, WritesAModuleLevelVar) {
  // A module-level `var` has storage the runtime owns, so that a program
  // half interpreted and half compiled still has one counter (D88).
  // Before this it lowered to a function that recomputed the initial
  // value, and `counter = counter + 1` compiled to nothing at all.
  Fixture f(R"(
var counter: int = 0
var label: int = 7

fun Bump() -> int {
  counter = counter + 1
  counter = counter + 1
  return counter * 10 + label
}
)");
  WOLANG_EXPECT_EQ(f.Run("Bump", {}), "27");
}

WOLANG_TEST(CodegenTest, BuildsAndMatchesAnEnumWithPayloads) {
  // An enum is a tag and room for the largest payload beside it (D89).
  // Which variant it is decides what the payload means, so the payload's
  // *type* is the instruction's rather than the declaration's -- that is
  // what `match` is for.
  Fixture f(R"(
struct Vec {
  var x: int = 0
  var y: int = 0
}

enum AiState {
  Idle
  Chase(target: int, speed: int)
  Patrol(at: Vec)
}

fun Describe(s: AiState) -> int {
  match s {
    AiState.Idle => { return 1 }
    AiState.Chase(target, speed) => { return target * 10 + speed }
    AiState.Patrol(at) => { return at.x * 100 + at.y }
  }
}

fun All() -> int {
  const idle = AiState.Idle
  const chase = AiState.Chase(7, 2)
  const patrol = AiState.Patrol(Vec(3, 4))
  // 1 + 72 + 304: a tag-only variant, two numbers, and a struct.
  return Describe(idle) + Describe(chase) + Describe(patrol)
}
)");
  WOLANG_EXPECT_EQ(f.Run("All", {}), "377");
}

WOLANG_TEST(CodegenTest, CarriesAnErrorThroughAFallible) {
  // `T!` is which it is and the value when it is a value (D90). An error
  // tag *is* its number, so the test and the construction agree without
  // either side knowing about the other -- and `?` hands the failure on
  // rather than making every caller write a `match`.
  Fixture f(R"(
error NotFound, Corrupt

fun Half(n: int) -> int!(Corrupt, NotFound) {
  if n % 2 != 0 {
    return Corrupt
  }
  return n / 2
}

fun Quarter(n: int) -> int!(Corrupt, NotFound) {
  const half = Half(n)?
  return Half(half)?
}

fun Use(n: int) -> int {
  match Quarter(n) {
    Ok(v) => { return v }
    Corrupt => { return -1 }
    NotFound => { return -2 }
  }
}

fun All() -> int {
  // Eight halves twice; six fails on the second half; three on the
  // first.
  return Use(8) * 100 + Use(6) * 10 + Use(3)
}
)");
  WOLANG_EXPECT_EQ(f.Run("All", {}), "189");
}

WOLANG_TEST(CodegenTest, PayloadErrorsAgree) {
  Fixture f(R"(
error Plain, Bad(line: int, why: text), Worse(code: int)

fun Make(n: int) -> int!(Plain, Bad, Worse) {
  if n == 0 {
    return Plain
  }
  if n == 1 {
    return Bad(40, "late")
  }
  if n == 2 {
    return Worse(7)
  }
  return n
}

fun Pass(n: int) -> int!(Plain, Bad, Worse) {
  const v = Make(n)?
  return v + 1
}

fun Narrow(n: int) -> int!(Bad) {
  if n == 1 {
    return Bad(5, "narrow")
  }
  return n
}

fun Widen(n: int) -> int!(Bad, Worse) {
  const v = Narrow(n)?
  return v * 2
}

fun Use(n: int) -> int {
  return match Pass(n) {
    Ok(v) => v
    Plain => -1
    Bad(line, why) => line + why.len
    Worse(code) => code * 100
  }
}

fun UseBare(n: int) -> int {
  return match Make(n) {
    Ok(v) => v
    Bad => 1000
    _ => 0
  }
}

fun UseWide(n: int) -> int {
  return match Widen(n) {
    Ok(v) => v
    Bad(line, why) => line
    Worse(code) => code
  }
}

fun Dropped() -> int {
  var total = 0
  var i = 0
  while i < 50 {
    const ignored = Make(1)
    total = total + 1
    i = i + 1
  }
  return total
}

fun All() -> int {
  return Use(0) + Use(1) * 10 + Use(2) + Use(3) + UseBare(1) + UseBare(2) + UseWide(1) + UseWide(4) + Dropped()
}
)");
  WOLANG_EXPECT_EQ(f.Run("Use", {Fixture::Int(1)}), "44");
  WOLANG_EXPECT_EQ(f.Run("Use", {Fixture::Int(2)}), "700");
  WOLANG_EXPECT_EQ(f.Run("Use", {Fixture::Int(0)}), "-1");
  WOLANG_EXPECT_EQ(f.Run("Use", {Fixture::Int(3)}), "4");
  WOLANG_EXPECT_EQ(f.Run("UseBare", {Fixture::Int(1)}), "1000");
  WOLANG_EXPECT_EQ(f.Run("UseBare", {Fixture::Int(2)}), "0");
  WOLANG_EXPECT_EQ(f.Run("UseWide", {Fixture::Int(1)}), "5");
  WOLANG_EXPECT_EQ(f.Run("UseWide", {Fixture::Int(4)}), "8");
  WOLANG_EXPECT_EQ(f.Run("Dropped", {}), "50");
}

WOLANG_TEST(CodegenTest, CarriesAValueThroughAnOptional) {
  Fixture f(R"(
struct Pawn {
  var hp: int = 1
}

fun Find(id: int) -> Pawn? {
  if id == 0 {
    return null
  }
  return Pawn(id * 10)
}

fun All() -> int {
  const present: int? = 5
  const absent: int? = null
  var total = present ?? 0
  total = total + (absent ?? 7)
  if const found = Find(2) {
    total = total + found.hp
  }
  // A literal into an optional, a default for an absent one, a binding
  // that only runs when there is something, and `?.` reaching through.
  return total + (Find(0)?.hp ?? -1) + (Find(3)?.hp ?? -1)
}
)");
  WOLANG_EXPECT_EQ(f.Run("All", {}), "61");
}

WOLANG_TEST(CodegenTest, BuildsAndComparesStrings) {
  // A string is a pointer and a length in every tier, and what is built
  // from one is built by the runtime both tiers call. Which is what makes
  // an interpolated string compile rather than drag the program it is in
  // into the interpreter.
  //
  // Equality compares the bytes. Comparing the pairs -- which is what
  // structural equality does, and what this used to do -- answers that two
  // copies of "GET" are different strings.
  Fixture f(R"(
fun Greeting(name: text) -> text => "hello, {name}"

fun Counted(n: int) -> text => "n={n}"

fun Same(a: text, b: text) -> bool => a == b

fun Differs(a: text, b: text) -> bool => a != b

fun SameLiterals() -> bool {
  const a = "GET"
  const b = "GET"
  return a == b
}

fun SameBuilt() -> bool {
  const built = "GE{"T"}"
  return built == "GET"
}
)");
  WOLANG_EXPECT_EQ(f.Run("Greeting", {Fixture::Text("world")}),
                   "\"hello, world\"");
  WOLANG_EXPECT_EQ(f.Run("Counted", {Fixture::Int(42)}), "\"n=42\"");
  WOLANG_EXPECT_EQ(f.Run("SameLiterals", {}), "true");
  WOLANG_EXPECT_EQ(f.Run("SameBuilt", {}), "true");
}

WOLANG_TEST(CodegenTest, ReturnsAnAggregateFromAFunctionThatCalls) {
  // The register an aggregate result is written through is one a callee
  // may clobber, so a function that returns an aggregate *and* calls
  // something has to keep that address itself. Every function that builds
  // an interpolated string is one, which is how this was found.
  Fixture f(R"(
struct Pair {
  var a: int
  var b: int
}

fun Twice(n: int) -> int => n * 2

fun Build(n: int) -> Pair {
  return Pair(Twice(n), Twice(n + 1))
}

fun Nested(n: int) -> Pair {
  const inner = Build(n)
  return Pair(inner.b, inner.a)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Build", {Fixture::Int(3)}), "(6, 8)");
  WOLANG_EXPECT_EQ(f.Run("Nested", {Fixture::Int(3)}), "(8, 6)");
}

WOLANG_TEST(CodegenTest, AnArenaPaysForItsAllocationsOnce) {
  // Everything allocated inside the block comes from one arena and the
  // whole arena goes at the end -- including when the way out is a
  // `return` or a `break`, which is why this is a deferred release in all
  // but name. What keeps a value from outliving it is `E0121`.
  Fixture f(R"(
extern "C" fun wolang_arena_depth() -> u64

class Node {
  var value: int = 0
}

fun Depth() -> int uses unsafe => wolang_arena_depth() as int

fun Nested() -> int uses unsafe {
  arena {
    arena {
      return Depth()
    }
  }
}

fun ReleasedAfterTheBlock() -> int uses unsafe {
  arena {
    const _node = Node(1)
  }
  return Depth()
}

fun ReleasedOnReturn() -> int uses unsafe {
  arena {
    const _node = Node(1)
    return 7
  }
}

fun ReleasedOnBreak() -> int uses unsafe {
  var total = 0
  var i = 0
  while i < 3 {
    arena {
      const node = Node(i)
      total = total + node.value
      if i == 1 {
        break
      }
    }
    i = i + 1
  }
  // The arena of the turn that broke out was released too.
  return total * 10 + Depth()
}

fun AllocatesManyInOne(n: int) -> int uses unsafe {
  var total = 0
  arena {
    var i = 0
    while i < n {
      const node = Node(i)
      total = total + node.value
      i = i + 1
    }
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Depth", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("Nested", {}), "2");
  WOLANG_EXPECT_EQ(f.Run("ReleasedAfterTheBlock", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("ReleasedOnReturn", {}), "7");
  WOLANG_EXPECT_EQ(f.Run("ReleasedOnBreak", {}), "10");
  WOLANG_EXPECT_EQ(f.Run("AllocatesManyInOne", {Fixture::Int(100)}),
                   "4950");
}

WOLANG_TEST(CodegenTest, HandsABufferToC) {
  // `&buf[2]` is an address into the buffer, not into a copy of it: a
  // `read` into a buffer is the whole shape of doing input, and it only
  // works if what C writes lands in the caller's bytes.
  //
  // The interpreter has no addresses for its values, so it copies the slot
  // out to real memory for the call and copies it back afterwards. That it
  // agrees with machine code here is the point of saying so.
  Fixture f(R"(
extern "C" fun memset(target: *mut u8, byte: s32, count: u64) -> *mut u8
extern "C" fun memcpy(target: *mut u8, source: *u8, count: u64) -> *mut u8

fun FillsPartOfABuffer() -> int uses unsafe {
  var buf: [8]u8
  const _r = memset(&buf[2], 65, 3)
  var total = 0
  for i in 0..8 {
    total = total + buf[i] as int
  }
  // Three bytes of 65 and five of zero, and the first two untouched.
  return total * 10 + buf[0] as int
}

fun CopiesBetweenBuffers() -> int uses unsafe {
  var source: [4]u8
  source[0] = 7
  source[1] = 9
  var target: [4]u8
  const _r = memcpy(&target[0], &source[0], 2)
  return target[0] as int * 100 + target[1] as int
}

fun WritesThroughAPointer() -> int uses unsafe {
  var buf: [4]int
  const p = &buf[1]
  *p = 42
  return buf[1]
}
)");
  WOLANG_EXPECT_EQ(f.Run("FillsPartOfABuffer", {}), "1950");
  WOLANG_EXPECT_EQ(f.Run("CopiesBetweenBuffers", {}), "709");
  WOLANG_EXPECT_EQ(f.Run("WritesThroughAPointer", {}), "42");
}

WOLANG_TEST(CodegenTest, FillsABufferThroughASlice) {
  // A buffer handed to a callee that writes it. Three things had to be
  // true at once for this to work, and each was wrong on its own:
  //
  //   * a slot read before anything wrote it holds zeroes, so slot
  //     promotion must leave it alone rather than call it undefined --
  //     which turned the buffer into an undefined value before the callee
  //     ever saw it;
  //   * writing an element through a slice changes the storage the slice
  //     views, so it is not an assignment to the binding;
  //   * a frame bigger than an instruction's immediate reach is still a
  //     frame, and a request buffer is four kilobytes on its own.
  Fixture f(R"(
fun Fill(bytes: []u8, value: u8) -> int {
  var written = 0
  while written < bytes.len {
    bytes[written] = value
    written = written + 1
  }
  return written
}

fun SumOf(bytes: []u8) -> int {
  var total = 0
  for b in bytes {
    total = total + b as int
  }
  return total
}

fun Small() -> int {
  var buffer: [4]u8
  const written = Fill(buffer, 3)
  return written * 100 + SumOf(buffer)
}

fun Large() -> int {
  var buffer: [4096]u8
  buffer[4095] = 9
  var total = 0
  for i in 0..4096 {
    total = total + buffer[i] as int
  }
  return total
}

fun ReadBeforeWriting() -> int {
  var buffer: [8]u8
  // Nothing wrote it, so every byte is zero -- in every tier.
  return SumOf(buffer) + buffer.len
}
)");
  WOLANG_EXPECT_EQ(f.Run("Small", {}), "412");
  WOLANG_EXPECT_EQ(f.Run("Large", {}), "9");
  WOLANG_EXPECT_EQ(f.Run("ReadBeforeWriting", {}), "8");
}

WOLANG_TEST(CodegenTest, ADerivedFieldRecomputesWhenAnInputChanges) {
  // "Recomputes when an input changes rather than on every read" is a
  // claim about *when*, so this is what it costs: a cached value, a bit
  // that says whether the cache is current, and a write to any field the
  // expression reads clearing that bit (D63).
  //
  // Which fields those are comes from the member table, not from this
  // file: the write may be in a module that has never seen the
  // expression.
  Fixture f(R"(
class Fighter {
  var hp: int = 10
  var buff: int = 2

  derived var power: int => hp * buff

  fun Hurt(amount: int) {
    hp = hp - amount
  }
}

fun FirstRead() -> int {
  const fighter = Fighter(10, 2)
  return fighter.power
}

fun AfterAWrite() -> int {
  const fighter = Fighter(10, 2)
  const before = fighter.power
  fighter.hp = 20
  return before * 100 + fighter.power
}

fun AfterAMethod() -> int {
  const fighter = Fighter(10, 2)
  const before = fighter.power
  fighter.Hurt(5)
  return before * 100 + fighter.power
}

fun ReadTwice() -> int {
  const fighter = Fighter(3, 4)
  // The second read is a load of the cache; both answer the same.
  return fighter.power + fighter.power
}
)");
  WOLANG_EXPECT_EQ(f.Run("FirstRead", {}), "20");
  WOLANG_EXPECT_EQ(f.Run("AfterAWrite", {}), "2040");
  // Ten hit for five is five, and five times two is ten.
  WOLANG_EXPECT_EQ(f.Run("AfterAMethod", {}), "2010");
  WOLANG_EXPECT_EQ(f.Run("ReadTwice", {}), "24");
}

WOLANG_TEST(CodegenTest, ASignalCallsWhatIsConnectedToIt) {
  // A signal's type is the type of what listens to it, so `Connect` and
  // `Emit` check against the same thing. Connecting is a call into the
  // runtime -- growing a list is the part that allocates -- and emitting
  // is a loop here, because calling back into the language is something
  // only the language can do.
  //
  // The interpreter has no addresses for its functions, so what it hands
  // the runtime is a token it can turn back into the value. That the two
  // tiers agree on all four lines below is the point.
  Fixture f(R"(
class Counter {
  var value: int = 0

  fun Bump(by: int) {
    value = value + by
  }
}

class Pawn {
  var hp: int = 10

  signal Died(witness: Counter)

  fun Kill(witness: Counter) {
    Died.Emit(witness)
  }
}

fun One(c: Counter) {
  c.Bump(1)
}

fun Ten(c: Counter) {
  c.Bump(10)
}

fun NoHandlers() -> int {
  const pawn = Pawn(10)
  const witness = Counter(0)
  pawn.Kill(witness)
  return witness.value
}

fun OneHandler() -> int {
  const pawn = Pawn(10)
  const witness = Counter(0)
  pawn.Died.Connect(One)
  pawn.Kill(witness)
  pawn.Kill(witness)
  return witness.value
}

fun TwoHandlers() -> int {
  const pawn = Pawn(10)
  const witness = Counter(0)
  pawn.Died.Connect(One)
  pawn.Died.Connect(Ten)
  pawn.Kill(witness)
  return witness.value
}

fun AfterDisconnect() -> int {
  const pawn = Pawn(10)
  const witness = Counter(0)
  pawn.Died.Connect(One)
  pawn.Died.Connect(Ten)
  pawn.Died.Disconnect(One)
  pawn.Kill(witness)
  return witness.value
}
)");
  WOLANG_EXPECT_EQ(f.Run("NoHandlers", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("OneHandler", {}), "2");
  WOLANG_EXPECT_EQ(f.Run("TwoHandlers", {}), "11");
  WOLANG_EXPECT_EQ(f.Run("AfterDisconnect", {}), "10");
}

WOLANG_TEST(CodegenTest, AMachineIsItsTransitionTable) {
  // A `machine` is a type whose value is which state it is in, and its
  // methods are what the table means: one per event, one per state, and
  // the blocks that run on the way in and out.
  //
  // `always` is followed when the target is entered, so one event can pass
  // through several states -- and every state on the way runs its `enter`.
  Fixture f(R"(
machine Lamp {
  state Off {
    on Flip -> Warming
  }
  state Warming {
    always -> On
  }
  state On {
    on Flip -> Off
  }
}

fun One(yes: bool) -> int {
  if yes {
    return 1
  }
  return 0
}

fun StartsInTheFirstState() -> int {
  const lamp = Lamp()
  return One(lamp.IsOff()) * 100 + One(lamp.IsOn()) * 10 +
         One(lamp.IsWarming())
}

fun FollowsAlways() -> int {
  const lamp = Lamp()
  const fired = lamp.Flip()
  // Off goes to Warming, and `always` carries it on to On.
  return One(fired) * 100 + One(lamp.IsOn()) * 10 +
         One(lamp.IsWarming())
}

fun ComesBack() -> int {
  const lamp = Lamp()
  const _first = lamp.Flip()
  const second = lamp.Flip()
  return One(second) * 10 + One(lamp.IsOff())
}

fun EveryFlipFires() -> int {
  const lamp = Lamp()
  const _first = lamp.Flip()
  const again = lamp.Flip()
  const third = lamp.Flip()
  return One(again) * 10 + One(third)
}
)");
  WOLANG_EXPECT_EQ(f.Run("StartsInTheFirstState", {}), "100");
  WOLANG_EXPECT_EQ(f.Run("FollowsAlways", {}), "110");
  WOLANG_EXPECT_EQ(f.Run("ComesBack", {}), "11");
  WOLANG_EXPECT_EQ(f.Run("EveryFlipFires", {}), "11");
}

WOLANG_TEST(CodegenTest, PassesArgumentsPastTheRegisters) {
  // Eight integer registers carry arguments on arm64 and six on x86-64,
  // and the dev tier indexed its table of them without checking -- so a
  // call with more arguments than that read whatever followed the table
  // and passed nonsense, in silence. What makes this a test rather than a
  // fixed bound is that the two machines have different bounds and the
  // third tier has none (D154).
  Fixture f(R"(
fun Ten(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int,
        i: int, j: int) -> int {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 +
         i * 9 + j * 10
}

fun TenFloats(a: float, b: float, c: float, d: float, e: float, f: float,
              g: float, h: float, i: float, j: float) -> float {
  return a + b * 2.0 + c * 3.0 + d * 4.0 + e * 5.0 + f * 6.0 + g * 7.0 +
         h * 8.0 + i * 9.0 + j * 10.0
}

fun Mixed(a: int, b: float, c: int, d: float, e: int, f: float, g: int,
          h: float, i: int, j: float, k: int, l: float) -> int {
  return a + c + e + g + i + k + (b + d + f + h + j + l) as int
}

fun Calls() -> int {
  return Ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
}

fun CallsWithFloats() -> int {
  return TenFloats(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
                   10.0) as int
}

fun CallsMixed() -> int {
  return Mixed(1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5)
}

fun NestedCalls() -> int {
  // A call inside a call: the outgoing area is reused, and the inner one
  // must be done with it before the outer one starts filling it.
  return Ten(Ten(1, 1, 1, 1, 1, 1, 1, 1, 1, 1), 2, 3, 4, 5, 6, 7, 8, 9,
             10)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Calls", {}), "385");
  WOLANG_EXPECT_EQ(f.Run("CallsWithFloats", {}), "385");
  WOLANG_EXPECT_EQ(f.Run("CallsMixed", {}), "45");
  WOLANG_EXPECT_EQ(f.Run("NestedCalls", {}), "439");
}

WOLANG_TEST(CodegenTest, TheAddressNoObjectHas) {
  // `ptrnull` is what a binding passes for NULL and tests what it got
  // back against. Before it there was no way to write one, so `lib/tls`
  // used a freed address as a sentinel -- which is a pointer that is not
  // null and not valid either (D156).
  Fixture f(R"(
extern "C" fun getenv(name: *u8) -> *mut u8
extern "C" fun strlen(s: *u8) -> u64

fun StartsOutNull() -> int uses unsafe {
  var found: *mut u8 = ptrnull
  if found != ptrnull {
    return 1
  }
  return 0
}

fun TellsThemApart() -> int uses unsafe {
  var name: [5]u8
  name[0] = 80
  name[1] = 65
  name[2] = 84
  name[3] = 72
  name[4] = 0
  const found = getenv(&name[0])
  if found == ptrnull {
    return 0
  }
  // Something is in PATH on every machine this runs on.
  if strlen(found) > 0 {
    return 1
  }
  return 1
}

fun AsANumber() -> int uses unsafe {
  const none: *mut u8 = ptrnull
  if none as u64 == 0 {
    return 7
  }
  return 8
}
)");
  WOLANG_EXPECT_EQ(f.Run("StartsOutNull", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("TellsThemApart", {}), "1");
  WOLANG_EXPECT_EQ(f.Run("AsANumber", {}), "7");
}

WOLANG_TEST(CodegenTest, AValueReturnedFromAFallible) {
  // `return 7` from a `-> int!` wraps: the checker records the literal
  // as `int!` already, so lowering built a `const.int` typed `int!` and
  // returned a bare seven where a pair was expected. The tiers then
  // read it as two different kinds of nonsense, which is what a
  // differential test is for (D182).
  Fixture f(R"(
error Broken

fun Risky(ok: bool) -> int!Broken {
  if !ok {
    return Broken
  }
  return 7
}

fun Floating(ok: bool) -> float!Broken {
  if !ok {
    return Broken
  }
  return 1.5
}

fun Works() -> int {
  match Risky(true) {
    Ok(value) => { return value },
    Broken => { return -1 },
  }
}

fun Fails() -> int {
  match Risky(false) {
    Ok(value) => { return value },
    Broken => { return -1 },
  }
}

fun FloatWorks() -> int {
  match Floating(true) {
    Ok(value) => { return (value * 2.0) as int },
    Broken => { return -1 },
  }
}
)");
  WOLANG_EXPECT_EQ(f.Run("Works", {}), "7");
  WOLANG_EXPECT_EQ(f.Run("Fails", {}), "-1");
  WOLANG_EXPECT_EQ(f.Run("FloatWorks", {}), "3");
}

WOLANG_TEST(CodegenTest, DividesByAConstantWithoutDividing) {
  // A constant divisor that is a power of two is a shift; signed needs
  // the bias that makes it round towards zero the way division does.
  // Every answer here is compared against the reference tier, which
  // still divides (D192).
  Fixture f(R"(
fun Eight(x: int) -> int => x / 8
fun RemEight(x: int) -> int => x % 8
fun MinusFour(x: int) -> int => x / -4
fun Ten(x: int) -> int => x % 10
fun One(x: int) -> int => x / 1
fun MinusOne(x: int) -> int => x / -1

fun Walk() -> int {
  var total = 0
  var i = -40
  while i <= 40 {
    total = total + Eight(i) + RemEight(i) * 3 + MinusFour(i) * 5 +
            Ten(i) * 7 + One(i) + MinusOne(i)
    i = i + 1
  }
  return total
}

fun Edges() -> int {
  // The rounding cases: just either side of zero and of the divisor.
  return Eight(-9) * 1000 + Eight(-8) * 100 + Eight(-7) * 10 +
         RemEight(-9)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Walk", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("Edges", {}), "-1101");
}

WOLANG_TEST(CodegenTest, DestroysASharedHandleOnce) {
  // Two names for one object: the destructor runs when the last of them
  // goes away, not when the first does. Before the count, the second
  // drop was a second free (D193).
  Fixture f(R"(
var gone: int = 0

class Noisy {
  var id: int = 0

  deinit {
    gone = gone + 1
  }
}

fun Borrows(noisy: Noisy) -> int {
  // A second holder, and a scope that ends before the caller's does.
  var copy = noisy
  return copy.id
}

fun Shares() -> int {
  gone = 0
  const id = Borrows(Noisy(7))
  // The object is gone exactly once by here.
  return id * 10 + gone
}

fun Nested() -> int {
  gone = 0
  var outer = 0
  const noisy = Noisy(3)
  outer = outer + Borrows(noisy)
  return outer * 10 + gone
}
)");
  WOLANG_EXPECT_EQ(f.Run("Shares", {}), "71");
  // The caller still holds `noisy` when it reads `gone`: borrowed, not
  // moved (D209).
  WOLANG_EXPECT_EQ(f.Run("Nested", {}), "30");
}

WOLANG_TEST(CodegenTest, AnInterpolationOfOneTextIsACopy) {
  // Found by the generator (D231): `"{name}"` was `name` itself, so the two
  // variables freed one allocation twice.
  Fixture f(R"(
fun Twice() -> int {
  var tx = "seed"
  var copy = "{tx}"
  tx = "{tx}!"
  return copy.len * 10 + tx.len
}
)");
  WOLANG_EXPECT_EQ(f.Run("Twice", {}), "45");
}

WOLANG_TEST(CodegenTest, CoalesceStoredInAnOptionalSlot) {
  // Found by the generator (D231): the `??` took the optional type its
  // context had, and its join was handed a bare value.
  Fixture f(R"(
fun Maybe(x: u8) -> u8? {
  if x > 100 {
    return null
  }
  return x
}

fun Kept(x: u8) -> int {
  var o: u8? = (Maybe(x) ?? 7)
  return (o ?? 0) as int
}

fun Small() -> int => Kept(5)
fun Large() -> int => Kept(200)
)");
  WOLANG_EXPECT_EQ(f.Run("Small", {}), "5");
  WOLANG_EXPECT_EQ(f.Run("Large", {}), "7");
}

WOLANG_TEST(CodegenTest, NarrowSignedResultsMayBeNegative) {
  // Found by the generator (D231): the 32-bit operation behind an s8 or
  // s16 `-`, `*` and negation leaves a negative result zero-extended, and
  // the fit check compared it with the sign-extended form and trapped.
  // The values come out of an array so that no pass folds them.
  Fixture f(R"(
fun Narrow() -> int {
  var buf: [4]int
  buf[0] = 5
  buf[1] = 7
  const a = buf[0] as s16
  const b = buf[1] as s16
  const c = buf[0] as s8
  const d = buf[1] as s8
  return (a - b) as int * 1000 + (-a) as int * 100 + (a * -b) as int +
         (c - d) as int * 10 + (-c) as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {}), "-2560");
}

WOLANG_TEST(CodegenTest, NarrowSignedDivisionByAPowerOfTwoRoundsToZero) {
  // Found by the generator (D231): the rounding bias of `x / 2^k` was
  // shifted by the type's width instead of the register's, so -1 / 2 in an
  // s16 or s8 was -1.
  Fixture f(R"(
fun Halves() -> int {
  var buf: [2]int
  buf[0] = 0 - 1
  buf[1] = 0 - 7
  const a = buf[0] as s16
  const b = buf[1] as s8
  return (a / 2) as int * 1000 + (b / 4) as int * 100 +
         (b % 4) as int * 10 + (a % 2) as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("Halves", {}), "-131");
}

WOLANG_TEST(CodegenTest, APayloadReadStaysInItsArm) {
  // Found by the generator (D231): the read of `Box`'s payload inside a
  // loop was hoisted out of the match that guarded it, and a `Dot` has none.
  Fixture f(R"(
enum Shape {
  Dot,
  Box(h: u64),
}

fun Arms() -> int {
  var x: u64 = 15
  var total = 0
  var i = 0
  while i < 3 {
    i = i + 1
    total = total + ((match Shape.Dot { Shape.Dot => x, Shape.Box(h) => h }) & 15) as int
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Run("Arms", {}), "45");
}

WOLANG_TEST(CodegenTest, FoldingAnUnsignedSumKeepsItsOverflowTrap) {
  // Found by the generator (D231): the folder read the u32 4294967294 as
  // -2, so the sum "fit" and the trap was folded away.
  Fixture f(R"(
fun Wraps() -> int {
  var x: u32 = 2147483647
  const y = (x + 4294967294) / 2
  return (y & 3) as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("Wraps", {}),
                   "trap: overflow at @Wraps:%5: add overflowed");
}

WOLANG_TEST(CodegenTest, ASignedThirtyTwoBitProductStillTrapsOnOverflow) {
  // The sign-extension that narrow `-` needs must not be applied to a
  // multiply's 64-bit product (D231).
  Fixture f(R"(
fun Product() -> s32 {
  var buf: [2]s32
  buf[0] = 0 - 2147483647
  const x = buf[0]
  const y = (~x) * x
  buf[1] = y
  return buf[1]
}
)");
  WOLANG_EXPECT_EQ(f.Run("Product", {}),
                   "trap: overflow at @Product:%13: mul overflowed");
}

WOLANG_TEST(CodegenTest, IntIsSixtyFourBitsInEveryTier) {
  // `int` is s64 (D238): 2^31 is a value, not an overflow, while s32 and the
  // top of s64 still trap.
  Fixture f(R"(
fun Grows() -> int {
  var a = 2147483647
  a += 1
  return a
}

fun Narrow() -> s32 {
  var a: s32 = 2147483647
  a += 1
  return a
}

fun Top() -> int {
  var a = 9223372036854775807
  a += 1
  return a
}
)");
  WOLANG_EXPECT_EQ(f.Run("Grows", {}), "2147483648");
  WOLANG_EXPECT_EQ(f.Run("Narrow", {}),
                   "trap: overflow at @Narrow:%5: add overflowed");
  WOLANG_EXPECT_EQ(f.Run("Top", {}),
                   "trap: overflow at @Top:%5: add overflowed");
}

WOLANG_TEST(CodegenTest, AHugeUnsignedDoesNotCastToANarrowSigned) {
  // Found by the generator (D231): `~20u64` has the bits of -21, and the
  // fit check compared bits, so the cast to s16 kept the low half.
  Fixture f(R"(
fun Huge() -> int {
  var buf: [2]u64
  buf[0] = 20
  const x = buf[0]
  const y = (~x) as s16
  return y as int
}
)");
  WOLANG_EXPECT_EQ(f.Run("Huge", {}),
                   "trap: overflow at @Huge:%11: the value does not fit the "
                   "integer it was cast to");
}

WOLANG_TEST(CodegenTest, ReleasesWhatObjectsHold) {
  Fixture f(R"(
extern "C" fun wolang_objects_live() -> s64

class Leaf {
  var id: int = 0
}

class Holder {
  var leaf: Leaf = Leaf(0)
  var maybe: Leaf? = null
}

class Node {
  var next: Node? = null
}

fun Live() -> int uses unsafe {
  return wolang_objects_live() as int
}

fun Keep(leaf: Leaf) -> int {
  return leaf.id
}

fun Reassigned() -> int uses unsafe {
  const before = Live()
  for i in 0..10 {
    var leaf = Leaf(i)
    leaf = Leaf(i + 1)
  }
  return Live() - before
}

fun Overwritten() -> int uses unsafe {
  const before = Live()
  for i in 0..10 {
    var holder = Holder(Leaf(0), null)
    const leaf = Leaf(i)
    holder.leaf = leaf
    holder.leaf = Leaf(i + 1)
    holder.maybe = Leaf(i)
    holder.maybe = null
    holder.maybe = leaf
  }
  return Live() - before
}

fun Passed() -> int uses unsafe {
  const before = Live()
  var total = 0
  for i in 0..10 {
    const leaf = Leaf(i)
    total = total + Keep(leaf) + Keep(Leaf(1))
  }
  return (Live() - before) * 1000 + total
}

fun Chained() -> int uses unsafe {
  const before = Live()
  var head = Node(null)
  for _i in 0..10 {
    head = Node(head)
  }
  return Live() - before
}

fun Survives() -> int {
  var holder = Holder(Leaf(0), null)
  const leaf = Leaf(42)
  holder.leaf = leaf
  holder.leaf = holder.leaf
  return holder.leaf.id + leaf.id
}

fun Chains() -> int uses unsafe {
  const before = Live()
  const _kept = Chained()
  return Live() - before
}
)");
  WOLANG_EXPECT_EQ(f.Run("Reassigned", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("Overwritten", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("Passed", {}), "55");
  WOLANG_EXPECT_EQ(f.Run("Chained", {}), "11");
  WOLANG_EXPECT_EQ(f.Run("Chains", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("Survives", {}), "84");
}

WOLANG_TEST(CodegenTest, ReleasesClassesHeldByValues) {
  Fixture f(R"(
extern "C" fun wolang_objects_live() -> s64

class Leaf {
  var id: int = 0
}

struct Pair {
  var left: Leaf
  var right: Leaf? = null
}

enum Shape {
  Empty
  One(leaf: Leaf)
  Two(pair: Pair)
}

fun Live() -> int uses unsafe {
  return wolang_objects_live() as int
}

fun Make(id: int) -> Shape {
  if id % 2 == 0 {
    return Shape.One(Leaf(id))
  }
  return Shape.Two(Pair(Leaf(id), Leaf(id + 1)))
}

fun Find(shape: Shape) -> Leaf? {
  return match shape {
    Shape.One(leaf) => leaf
    Shape.Two(pair) => pair.right
    Shape.Empty => null
  }
}

fun Copied() -> int uses unsafe {
  const was = Live()
  var total = 0
  for i in 0..20 {
    const shape = Make(i)
    const copy = shape
    var pair = Pair(Leaf(1), null)
    pair.right = Leaf(2)
    pair.left = pair.right ?? Leaf(3)
    total = total + (Find(copy)?.id ?? 0) + (Find(Make(i))?.id ?? 0)
  }
  return (Live() - was) * 1000 + total
}

fun Counted() -> int uses unsafe {
  const was = Live()
  var result: Leaf? = null
  for i in 0..5 {
    result = Find(Make(i))
  }
  const kept = result?.id ?? -1
  return (Live() - was) * 100 + kept
}
)");
  WOLANG_EXPECT_EQ(f.Run("Copied", {}), "400");
  WOLANG_EXPECT_EQ(f.Run("Counted", {}), "104");
}

WOLANG_TEST(CodegenTest, WalksThroughAssociatedTypes) {
  Fixture f(R"(
ability Walks {
  type Item
  fun Next() -> bool
  fun Value() -> Item
}

ability Walkable {
  type Walker: Walks
  fun Iterate() -> Walker
}

class Range {
  var from: int = 0
  var to: int = 0
}

class RangeWalk {
  var at: int = 0
  var to: int = 0
}

extend RangeWalk as Walks {
  type Item = int
  fun Next() -> bool {
    at = at + 1
    return at < to
  }
  fun Value() -> int => at
}

extend Range as Walkable {
  type Walker = RangeWalk
  fun Iterate() -> RangeWalk => RangeWalk(from - 1, to)
}

class Box<T> {
  var held: T
}

class BoxWalk<T> {
  var held: T
  var done: bool = false
}

extend<T> BoxWalk<T> as Walks {
  type Item = T
  fun Next() -> bool {
    const was = done
    done = true
    return !was
  }
  fun Value() -> T => held
}

extend<T> Box<T> as Walkable {
  type Walker = BoxWalk<T>
  fun Iterate() -> BoxWalk<T> => BoxWalk(held, false)
}

fun Count<C: Walkable>(items: C) -> int {
  var total = 0
  for _x in items {
    total = total + 1
  }
  return total
}

fun First<C: Walkable>(items: C) -> C.Walker.Item? {
  var walker = items.Iterate()
  if walker.Next() {
    return walker.Value()
  }
  return null
}

fun Sum() -> int {
  var total = 0
  for x in Range(1, 5) {
    total = total + x
  }
  return total
}

fun Counts() -> int {
  return Count(Range(1, 5)) * 10 + Count(Box(42))
}

fun Firsts() -> int {
  return (First(Range(7, 9)) ?? -1) * 100 + (First(Box(9)) ?? -1)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Sum", {}), "10");
  WOLANG_EXPECT_EQ(f.Run("Counts", {}), "41");
  WOLANG_EXPECT_EQ(f.Run("Firsts", {}), "709");
}

WOLANG_TEST(CodegenTest, AWeakFieldDoesNotKeepItsObject) {
  Fixture f(R"(
extern "C" fun wolang_objects_live() -> s64

class Parent {
  var child: Child? = null
  var id: int = 0
}

class Child {
  weak var parent: Parent? = null
}

fun Live() -> int uses unsafe {
  return wolang_objects_live() as int
}

fun Cycle() -> int uses unsafe {
  const before = Live()
  for i in 0..10 {
    var parent = Parent(null, i)
    const child = Child(parent)
    parent.child = child
  }
  return Live() - before
}

fun Reads() -> int {
  var parent = Parent(null, 7)
  const child = Child(null)
  child.parent = parent
  parent.child = child
  return child.parent?.id ?? -1
}

fun Outlives() -> int {
  const child = Child(null)
  child.parent = Parent(null, 9)
  return child.parent?.id ?? -1
}

fun Stays() -> int uses unsafe {
  const before = Live()
  const child = Child(null)
  var kept = 0
  for i in 0..5 {
    const parent = Parent(null, i)
    child.parent = parent
    kept = kept + (child.parent?.id ?? 100)
  }
  return (Live() - before) * 1000 + kept + (child.parent?.id ?? 100)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Cycle", {}), "0");
  WOLANG_EXPECT_EQ(f.Run("Reads", {}), "7");
  WOLANG_EXPECT_EQ(f.Run("Outlives", {}), "-1");
  WOLANG_EXPECT_EQ(f.Run("Stays", {}), "1110");
}

WOLANG_TEST(CodegenTest, CallsAFunctionWithMoreArgumentsThanRegisters) {
  // A host call to a function whose arguments do not all fit in
  // registers. The body has read the rest off the stack since D154; the
  // trampoline that a host enters through wrote past the end of its
  // register table until the same was done there.
  Fixture f(R"(
fun Eleven(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int,
           i: int, j: int, k: int) -> int {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 +
         i * 9 + j * 10 + k * 11
}

fun Mixed(a: f64, b: int, c: f64, d: int, e: f64, f: int, g: f64, h: int,
          i: f64, j: int, k: f64, l: int) -> int {
  // Integers out, so that what comes back is exact.
  return a as int + b + c as int + d + e as int + f + g as int + h +
         i as int + j + k as int + l
}
)");
  WOLANG_EXPECT_EQ(f.Run("Eleven", {Fixture::Int(1), Fixture::Int(1),
                                    Fixture::Int(1), Fixture::Int(1),
                                    Fixture::Int(1), Fixture::Int(1),
                                    Fixture::Int(1), Fixture::Int(1),
                                    Fixture::Int(1), Fixture::Int(1),
                                    Fixture::Int(1)}),
                   "66");
  WOLANG_EXPECT_EQ(f.Run("Eleven", {Fixture::Int(0), Fixture::Int(0),
                                    Fixture::Int(0), Fixture::Int(0),
                                    Fixture::Int(0), Fixture::Int(0),
                                    Fixture::Int(0), Fixture::Int(0),
                                    Fixture::Int(0), Fixture::Int(0),
                                    Fixture::Int(3)}),
                   "33");
  WOLANG_EXPECT_EQ(f.Run("Mixed", {Fixture::Float(1.0), Fixture::Int(2),
                                   Fixture::Float(3.0), Fixture::Int(4),
                                   Fixture::Float(5.0), Fixture::Int(6),
                                   Fixture::Float(7.0), Fixture::Int(8),
                                   Fixture::Float(9.0), Fixture::Int(10),
                                   Fixture::Float(11.0), Fixture::Int(12)}),
                   "78");
}

WOLANG_TEST(CodegenTest, CarriesASliceAndACounterAroundALoop) {
  // Values that trade places around a loop go through swap slots, and a
  // slice needs a slot wider than a word: in eight bytes it keeps its
  // address and loses its length. One back end was told that and the
  // other was not, so this walked an array on one machine and indexed
  // past the end of it on the other (D200).
  Fixture f(R"(
fun Fill(out: []u8, count: int) -> int {
  var written = 0
  var i = 0
  while i < count {
    const digit = (i * 3) % 7
    i = i + 1
    if digit == 0 {
      // The `continue` is what makes the loop carry its values round
      // rather than fall through with them.
      continue
    }
    out[written] = digit as u8
    written = written + 1
  }
  // The length as the loop left it, and what was written through it.
  var total = out.len * 1000 + written * 10
  if out.len > 0 {
    total = total + out[0] as int
  }
  return total
}

fun Walks() -> int {
  var buffer: [11]u8
  return Fill(buffer, 11)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Walks", {}), "11093");
}

// Literals wider than `int` keep their value in every tier (D225).
WOLANG_TEST(CodegenTest, WideLiteralsKeepTheirValue) {
  Fixture f(R"(
fun Wide() -> int {
  const g = 4294967297 as u64
  const n = -4294967297 as s64
  const c: s64 = -9223372036854775807 - 1
  const e: s64 = 9223372036854775807 - 1
  var r = 0
  if g == 4294967297 { r = r + 1 }
  if n == -4294967297 { r = r + 2 }
  if c < 0 { r = r + 4 }
  if e > 0 { r = r + 8 }
  return r
}
)");
  WOLANG_EXPECT_EQ(f.Run("Wide", {}), "15");
}

// A recursive enum is E0117 in sema; the layout must still answer rather
// than recurse until the stack ends if one gets past it (D225).
WOLANG_TEST(CodegenTest, LayoutSurvivesARecursiveEnum) {
  Database db;
  const FileId file = db.AddFile("<test>", "enum E {\n  X(e: E)\n  Y\n}\n");
  const CheckedFile* checked = db.Checked(file);
  WOLANG_ASSERT(checked != nullptr);
  const Binding* binding =
      checked->resolution.Find(db.interner().Intern("E"));
  WOLANG_ASSERT(binding != nullptr);
  abi::LayoutStore layouts(db.types(), checked->resolution);
  WOLANG_EXPECT(!layouts.IsSupported(binding->type));
}

WOLANG_TEST(CodegenTest, ADeinitThatKeepsSelfStopsEveryTier) {
  // Storing `self` from inside its own `deinit` leaves a freed object
  // referenced, so every tier stops at the same place (D229). Sema only
  // sees the direct assignment; a call that stores it is the runtime's.
  Fixture f(R"(
var stash: Zombie? = null

fun Keep(z: Zombie) {
  stash = z
}

class Zombie {
  var id: int = 0
  deinit {
    Keep(self)
  }
}

fun Make() -> int {
  const z = Zombie(1)
  return z.id
}

class Fine {
  var id: int = 0
}

fun MakeFine() -> int {
  const z = Fine(2)
  return z.id
}
)");
  WOLANG_EXPECT_EQ(f.Run("MakeFine", {}), "2");
  const std::string trapped = f.Run("Make", {});
  WOLANG_EXPECT(trapped.starts_with("trap: unreachable at"));
}

WOLANG_TEST(CodegenTest, AnExpressionBodyDropsItsOwningParameter) {
  // `=> r.id` left the function without the destroy a `return` runs, so a
  // value handed in by move was never dropped (D229).
  Fixture f(R"(
var drops: int = 0

struct Res {
  var id: int
  deinit {
    drops = drops + 1
  }
}

fun Take(r: Res) -> int => r.id

fun Once() -> int {
  drops = 0
  const r = Res(1)
  const _x = Take(r)
  return drops
}
)");
  WOLANG_EXPECT_EQ(f.Run("Once", {}), "1");
}

WOLANG_TEST(CodegenTest, ATextLiteralReturnedAsAnOptionalIsWrapped) {
  // The literal was built typed `text?`, which Coerce took for already
  // wrapped; the interpreter read none and the JIT read garbage (D100).
  Fixture f(R"(
fun Pick(n: int) -> text? {
  if n > 1 {
    return "found"
  }
  return null
}

fun Check(n: int) -> int {
  const s = Pick(n) ?? "none"
  if s == "found" {
    return 1
  }
  return 0
}
)");
  WOLANG_EXPECT_EQ(f.Run("Check", {Fixture::Wide(3)}), "1");
  WOLANG_EXPECT_EQ(f.Run("Check", {Fixture::Wide(0)}), "0");
}

WOLANG_TEST(CodegenTest, AnErrorWithAWidePayloadKeepsTheCallersRegisters) {
  // An error of a `T!` whose `T` is an aggregate was built with an integer
  // constant as its payload. The back ends gave that constant no register
  // and no spill slot, and storing it wrote just below the spill area --
  // over a saved register, which the caller's loop was keeping a tag in
  // (D249). Found on arm64; x64 wrote the same way into its own frame.
  Fixture f(R"(
error Broken

struct Wide {
  var a: int
  var b: int
  var c: int
}

fun Make(n: int, d: int, e: int, g: int, h: int) -> Wide!Broken {
  // Enough live at once that every register the callee may use is saved.
  const p = n + d
  const q = p * 2 + e
  const r = q * 3 + g
  const s = r * 5 + h
  const t = s * 7 + p
  const u = t * 11 + q
  const v = u * 13 + r
  const w = v * 17 + s
  if n > 0 && p + q + r + s + t + u + v + w != 0 {
    return Broken
  }
  return Wide(d, e, g + h + p + q + r + s + t + u + v + w - (p + q + r + s + t + u + v + w))
}

fun Refusals() -> int {
  var refused = 0
  var i = 0
  // Nine values live across the call, so the caller holds one in every
  // callee-saved register the pool has.
  const a = 1
  const b = 2
  const c = 3
  const d = 4
  const e = 5
  const g = 6
  const h = 7
  const k = 8
  const m = 9
  while i < 3 {
    match Make(1, i, i, i, i) {
      Ok(_w) => refused = refused
      Broken => refused = refused + a + b + c + d + e + g + h + k + m - 44
    }
    i = i + 1
  }
  match Make(0, 1, 2, 3, 4) {
    Ok(w) => refused = refused * 100 + w.c
    Broken => refused = -1
  }
  return refused
}
)");
  WOLANG_EXPECT_EQ(f.Run("Refusals", {}), "307");
}

WOLANG_TEST(CodegenTest, TextOrderingAgreesInEveryTier) {
  // `<` on text calls wolang_text_compare; the interpreter needs the same
  // intrinsic as the machine code (D226).
  Fixture f(R"(
fun Order(a: text, b: text) -> int {
  var n = 0
  if a < b { n = n + 1 }
  if a >= b { n = n + 10 }
  return n
}

fun Check() -> int => Order("a", "b") * 100 + Order("b", "a") + Order("a", "ab")
)");
  WOLANG_EXPECT_EQ(f.Run("Check", {}), "111");
}

WOLANG_TEST(CodegenTest, FloatLiteralArithmeticIsDoneAtTheTypeWanted) {
  // `1.0 / 3.0` where an f64 is wanted was divided as the default float and
  // widened (D225). A tree of integers alone stays integer arithmetic.
  Fixture f(R"(
fun Third() -> f64 {
  return 1.0 / 3.0
}

fun Mixed() -> f64 => 2.0 / 3.0 + 1.0

fun Whole() -> f64 => 7 / 2 as f64

fun Single() -> f32 => 1.0 / 3.0
)");
  WOLANG_EXPECT_EQ(f.Run("Third", {}), "0.3333333333333333");
  WOLANG_EXPECT_EQ(f.Run("Mixed", {}), "1.6666666666666665");
}

WOLANG_TEST(CodegenTest, AClosureReadingAnOwningValueLeavesItsDropToTheOwner) {
  // The closure borrows; the function around it drops once (D233).
  Fixture f(R"(
var drops: int = 0

struct Res {
  var id: int
  deinit {
    drops = drops + 1
  }
}

fun Run() -> int {
  drops = 0
  {
    const r = Res(3)
    const g = || r.id
    const _y = g()
  }
  return drops
}
)");
  WOLANG_EXPECT_EQ(f.Run("Run", {}), "1");
}

WOLANG_TEST(CodegenTest, AnEnumAndAStructDropWhatTheyCarry) {
  // An enum whose payload no arm moves, and a struct holding a value with a
  // `deinit`, ran no `deinit` at all; their end drops the carried value
  // once, and an arm that moves the payload on drops it once too (D233).
  Fixture f(R"(
var drops: int = 0

struct Res {
  var id: int
  deinit {
    drops = drops + 1
  }
}

struct Outer {
  var r: Res
}

enum Wrap {
  None
  Has(r: Res)
}

fun Take(r: Res) -> int => r.id

fun Unmatched() -> int {
  drops = 0
  const _w = Wrap.Has(Res(1))
  return 0
}

fun Left() -> int {
  drops = 0
  {
    const _w = Wrap.Has(Res(1))
  }
  return drops
}

fun InStruct() -> int {
  drops = 0
  {
    const _o = Outer(Res(1))
  }
  return drops
}

fun Moved() -> int {
  drops = 0
  {
    const w = Wrap.Has(Res(1))
    const _n = match w {
      Wrap.Has(r) => Take(r),
      Wrap.None => 0,
    }
  }
  return drops
}

fun Borrowed() -> int {
  drops = 0
  {
    const w = Wrap.Has(Res(1))
    const _n = match w {
      Wrap.Has(r) => r.id,
      Wrap.None => 0,
    }
  }
  return drops
}

fun Copied() -> int {
  drops = 0
  {
    const r = Res(4)
    const w = Wrap.Has(r)
    const _c = w
  }
  return drops
}
)");
  WOLANG_EXPECT_EQ(f.Run("Left", {}), "1");
  WOLANG_EXPECT_EQ(f.Run("InStruct", {}), "1");
  WOLANG_EXPECT_EQ(f.Run("Moved", {}), "1");
  WOLANG_EXPECT_EQ(f.Run("Borrowed", {}), "1");
  WOLANG_EXPECT_EQ(f.Run("Copied", {}), "1");
}

WOLANG_TEST(CodegenTest, CallThroughModuleVarOfFunctionTypeAgrees) {
  // `saved(1)` resolved to the constant initializer `K$saved`, which
  // takes no arguments, and reading `saved` took that function's address
  // instead of loading the variable (D240).
  Fixture f(R"(
fun Inc(x: int) -> int => x + 1
fun Twice(x: int) -> int => x * 2
var saved: fun(int) -> int = Inc

fun Check() -> int {
  const first = saved(1)
  saved = Twice
  return first * 100 + saved(5)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Check", {}), "210");
}

WOLANG_TEST(CodegenTest, CompoundAssignmentOperatesOnItsTarget) {
  // `i += 1` lowered as `i = 1`, so a loop on it never ended (D246).
  Fixture f(R"(
offer fun ArrayNew<T>(count: int) -> []T uses unsafe
offer fun SoaNew<T>(count: int) -> soa []T uses unsafe

var total: int = 10

struct Pair {
  var a: int = 1
  var b: int = 2
}

class Box {
  var n: int = 5
}

struct Particle {
  var x: float = 0.0
  var life: int = 0
}

var calls: int = 0

fun Next() -> int {
  calls += 1
  return 1
}

fun Bump(p: &mut int) {
  *p += 4
}

fun Loop() -> int {
  var i = 0
  while i < 5 {
    i += 1
  }
  return i
}

fun Locals() -> int {
  var a = 10
  a += 5
  a -= 3
  a *= 4
  a /= 3
  a %= 7
  var b = 12
  b &= 10
  b |= 1
  b ^= 3
  b <<= 2
  b >>= 1
  return a * 1000 + b
}

fun Globals() -> int {
  total += 5
  total *= 2
  return total
}

fun Fields() -> int {
  var p = Pair(1, 2)
  p.a += 10
  p.b *= 7
  const box = Box(5)
  box.n -= 2
  return p.a * 100 + p.b + box.n * 10000
}

fun Elements() -> int uses unsafe {
  const xs = ArrayNew<int>(3)
  xs[0] = 1
  xs[1] = 2
  xs[2] = 3
  xs[Next()] += 40
  xs[2] <<= 2
  return xs[0] + xs[1] * 10 + xs[2] * 1000 + calls * 100000
}

fun ThroughReference() -> int {
  var x = 3
  Bump(&mut x)
  return x
}

fun SoaFields() -> int uses unsafe {
  const ps = SoaNew<Particle>(2)
  ps[1].life += 7
  ps[1].life += 7
  ps[1].x += 1.5
  return ps[1].life + (ps[1].x * 2.0) as int
}

fun Floats() -> float {
  var f = 1.5
  f += 2.0
  f *= 2.0
  f -= 1.0
  f /= 2.0
  return f
}

fun Joined() -> int {
  var s = "ab"
  s += "cd"
  return s.length as int
}

fun Coalesced() -> int {
  var empty: int? = null
  empty ??= 9
  var full: int? = 4
  full ??= Next() + 50
  return (empty ?? 0) * 10 + (full ?? 0) + calls * 1000
}

fun Overflow() -> s32 {
  var a: s32 = 2147483647
  a += 1
  return a
}

fun DivideByZero(z: int) -> int {
  var a = 4
  a /= z
  return a
}

fun ShiftTooFar(n: int) -> int {
  var a = 1
  a <<= n
  return a
}
)");
  WOLANG_EXPECT_EQ(f.Run("Loop", {}), "5");
  WOLANG_EXPECT_EQ(f.Run("Locals", {}), "2020");
  WOLANG_EXPECT_EQ(f.Run("Globals", {}), "30");
  WOLANG_EXPECT_EQ(f.Run("Fields", {}), "31114");
  WOLANG_EXPECT_EQ(f.Run("Elements", {}), "112421");
  WOLANG_EXPECT_EQ(f.Run("ThroughReference", {}), "7");
  WOLANG_EXPECT_EQ(f.Run("SoaFields", {}), "17");
  WOLANG_EXPECT_EQ(f.Run("Floats", {}), "3");
  WOLANG_EXPECT_EQ(f.Run("Joined", {}), "4");
  WOLANG_EXPECT_EQ(f.Run("Coalesced", {}), "94");
  WOLANG_EXPECT(f.Run("Overflow", {}).find("trap:") == size_t{0});
  WOLANG_EXPECT(f.Run("DivideByZero", {Fixture::Wide(0)}).find("trap:") ==
                size_t{0});
  WOLANG_EXPECT(f.Run("ShiftTooFar", {Fixture::Wide(70)}).find("trap:") ==
                size_t{0});
}

static constexpr const char kOperatorPrelude[] = R"(
@Operator
ability Add {
  fun Plus(other: Self) -> Self
}

@Operator
ability Sub {
  fun Minus(other: Self) -> Self
}

@Operator
ability Mul {
  fun Times(other: Self) -> Self
}

@Operator
ability Div {
  fun Divide(other: Self) -> Self
}

@Operator
ability Neg {
  fun Negate() -> Self
}

@Operator
ability Eq {
  fun Equals(other: &Self) -> bool
}

@Operator
ability Ord {
  fun Compare(other: &Self) -> int
}

struct V {
  var x: int
  var y: int
}

extend V as Add {
  fun Plus(other: Self) -> Self => V(x + other.x, y + other.y)
}

extend V as Sub {
  fun Minus(other: Self) -> Self => V(x - other.x, y - other.y)
}

extend V as Mul {
  fun Times(other: Self) -> Self => V(x * other.x, y * other.y)
}

extend V as Div {
  fun Divide(other: Self) -> Self => V(x / other.x, y / other.y)
}

extend V as Neg {
  fun Negate() -> Self => V(-x, -y)
}

extend V as Eq {
  fun Equals(other: &Self) -> bool => x == other.x && y == other.y
}

extend V as Ord {
  fun Compare(other: &Self) -> int => x - other.x
}

class Money {
  var cents: int
}

extend Money as Add {
  fun Plus(other: Self) -> Self => Money(cents + other.cents)
}

extend Money as Eq {
  fun Equals(other: &Self) -> bool => cents == other.cents
}

fun Max<T: Ord>(a: T, b: T) -> T {
  if a >= b {
    return a
  }
  return b
}

fun Sign<T: Ord>(a: T, b: T) -> int => a.Compare(&b)
)";

// D239: an operator on a type of your own is a call, and every tier agrees on
// its answer.
WOLANG_TEST(CodegenTest, OperatorsOnStructsAreCallsInEveryTier) {
  Fixture f(std::string(kOperatorPrelude) + R"(
fun Arithmetic() -> int {
  const a = V(12, 30)
  const b = V(3, 5)
  const sum = a + b
  const less = a - b
  const more = a * b
  const part = a / b
  const flipped = -a
  return sum.x + sum.y * 10 + less.x * 100 + more.y + part.x + flipped.y
}

fun Equal() -> int {
  const a = V(1, 2)
  const b = V(1, 2)
  const c = V(1, 3)
  var n = 0
  if a == b { n += 1 }
  if a != c { n += 10 }
  if a == c { n += 100 }
  if a != b { n += 1000 }
  return n
}

fun Ordered() -> int {
  const small = V(1, 0)
  const big = V(2, 0)
  var n = 0
  if small < big { n += 1 }
  if small <= big { n += 2 }
  if big > small { n += 4 }
  if big >= small { n += 8 }
  if small > big { n += 16 }
  if big <= small { n += 32 }
  if small <= V(1, 5) { n += 64 }
  return n
}

fun InPlace() -> int {
  var total = V(1, 1)
  total += V(2, 3)
  total -= V(1, 1)
  return total.x * 10 + total.y
}

fun Classes() -> int {
  const a = Money(250)
  const b = Money(175)
  var total = a + b
  total += Money(5)
  var n = total.cents
  if a != b { n += 1000 }
  return n + a.cents - 250
}

fun Generic() -> int {
  const big = Max(V(1, 0), V(4, 0))
  return big.x * 100 + Max(3, 9) * 10 + Sign(2, 7) + Sign(7, 2) * 5 + Sign(4, 4)
}
)");
  WOLANG_EXPECT_EQ(f.Run("Arithmetic", {}), "1389");
  WOLANG_EXPECT_EQ(f.Run("Equal", {}), "11");
  WOLANG_EXPECT_EQ(f.Run("Ordered", {}), "79");
  WOLANG_EXPECT_EQ(f.Run("InPlace", {}), "23");
  WOLANG_EXPECT_EQ(f.Run("Classes", {}), "1430");
  WOLANG_EXPECT_EQ(f.Run("Generic", {}), "494");
}

}  // namespace
}  // namespace codegen
}  // namespace wolang
