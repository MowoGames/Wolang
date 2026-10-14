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
    if (Describe(reference) != Describe(optimized)) {
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

}  // namespace
}  // namespace codegen
}  // namespace wolang
