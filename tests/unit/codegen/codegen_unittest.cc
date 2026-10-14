// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/compiler.h"

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
      return left.ok() == right.ok() && left.trap == right.trap;
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
fun Widen(n: s32) -> s64 => n as s64

fun Narrow(n: s32) -> u8 => n as u8

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

WOLANG_TEST(CodegenTest, SaysWhatItCannotCompile) {
  // A tier that compiles what it can has to say what it did not, or "why
  // is this slow" has no answer.
  Fixture f(R"(
fun Greet(name: text) -> Text => "hello, {name}"

fun Count(n: int) -> int => n + 1
)");
  // An owned string means allocating one, and this tier has no heap.
  WOLANG_EXPECT_EQ(f.Skipped("Greet"), "returns `Text`");
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
  WOLANG_EXPECT_EQ(f.Skipped("Larger<s32>"), "");
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

fun Magnitude(n: int) -> int => abs(n)

fun Distance(x: f64) -> f64 => fabs(x)
)");
  WOLANG_EXPECT_EQ(f.Run("Magnitude", {Fixture::Int(-7)}), "7");
  WOLANG_EXPECT_EQ(f.Run("Magnitude", {Fixture::Int(7)}), "7");
  WOLANG_EXPECT_EQ(f.Run("Distance", {Fixture::Double(-2.5)}), "2.5");
}

WOLANG_TEST(CodegenTest, SaysWhenAForeignSymbolIsMissing) {
  Fixture f(R"(
extern "C" fun NotInAnyLibrary(n: s32) -> s32

fun Try(n: int) -> int => NotInAnyLibrary(n)
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
  *block = n + 1
  const read = *block
  free(block)
  return read
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
  *block = n + 1
  return *block
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
  // Took ownership: this scope destroys it.
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
  // One: every run starts from the type's zero, so what this counts is
  // the single destruction `Take` did (D88).
  return size * 100 + destroyed
}
)");
  WOLANG_EXPECT_EQ(f.Run("Counted", {}), "3");
  WOLANG_EXPECT_EQ(f.Run("Moved", {}), "701");
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

fun Half(n: int) -> int! {
  if n % 2 != 0 {
    return Corrupt
  }
  return n / 2
}

fun Quarter(n: int) -> int! {
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

}  // namespace
}  // namespace codegen
}  // namespace wolang
