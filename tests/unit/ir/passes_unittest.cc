// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/passes.h"

#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/compiler/database.h"
#include "wolang/ir/printer.h"
#include "wolang/ir/verifier.h"

namespace wolang {
namespace {

// Runs a snippet through lowering and the passes, and keeps both forms. A
// pass that breaks an invariant and a lowering that breaks one are different
// bugs, and the test has to be able to say which.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  std::string Problems() {
    const LoweredFile* lowered = db_.Lowered(file_);
    if (lowered == nullptr) {
      return "<none>";
    }
    std::string result;
    for (const std::string& problem : lowered->problems) {
      if (!result.empty()) {
        result.push_back('\n');
      }
      result.append(problem);
    }
    return result;
  }

  std::string Optimized(std::string_view name) {
    return Extract(Print(/*optimized=*/true), name);
  }
  std::string Lowered(std::string_view name) {
    return Extract(Print(/*optimized=*/false), name);
  }

 private:
  std::string Print(bool optimized) {
    const LoweredFile* lowered = db_.Lowered(file_);
    if (lowered == nullptr) {
      return "<none>";
    }
    return ir::PrintModule(
        db_.interner(), db_.types(),
        optimized ? lowered->module : lowered->unoptimized);
  }

  static std::string Extract(const std::string& all, std::string_view name) {
    std::string needle = "fun @";
    needle.append(name);
    needle.push_back('(');
    const size_t begin = all.find(needle);
    if (begin == std::string::npos) {
      return "<not found>";
    }
    const size_t end = all.find("\n}\n", begin);
    return end == std::string::npos ? all.substr(begin)
                                    : all.substr(begin, end - begin + 3);
  }

  Database db_;
  FileId file_;
};

WOLANG_TEST(PassTest, FoldsConstantArithmetic) {
  Fixture f("fun Folded() -> int {\n  return 2 + 3 * 4\n}\n");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Optimized("Folded");
  WOLANG_EXPECT(body.find("const.int 14") != std::string::npos);
  WOLANG_EXPECT(body.find("add") == std::string::npos);
  WOLANG_EXPECT(body.find("mul") == std::string::npos);
}

WOLANG_TEST(PassTest, DoesNotFoldDivisionByZero) {
  // A fault at runtime is the right answer; a wrong constant is not.
  Fixture f("fun Bad() -> int {\n  return 1 / 0\n}\n");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Optimized("Bad").find("div") != std::string::npos);
}

WOLANG_TEST(PassTest, FoldsComparisonsAndNegation) {
  Fixture f(R"(
fun Compare() -> bool {
  return 3 > 2
}

fun Flip() -> bool {
  return !true
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Optimized("Compare").find("const.bool true") !=
                std::string::npos);
  WOLANG_EXPECT(f.Optimized("Flip").find("const.bool false") !=
                std::string::npos);
}

WOLANG_TEST(PassTest, RemovesAnUnusedComputationThatCannotTrap) {
  Fixture f(R"(
fun Dead(n: int) -> int {
  const _unused = n > 0
  return n
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  // Present before the passes, gone after: that is the pass doing the work
  // rather than lowering never emitting it.
  WOLANG_EXPECT(f.Lowered("Dead").find("gt") != std::string::npos);
  WOLANG_EXPECT(f.Optimized("Dead").find("gt") == std::string::npos);
}

WOLANG_TEST(PassTest, KeepsUnusedArithmeticBecauseItCanTrap) {
  // Overflow is a trap, and a trap is something the program can observe, so
  // deleting an unused `n + n` would turn a program that faults into one
  // that does not. Removing it needs a proof that it cannot trap.
  Fixture f(R"(
fun Kept(n: int) -> int {
  const _unused = n + n
  return n
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Optimized("Kept").find("add") != std::string::npos);
}

WOLANG_TEST(PassTest, KeepsACallWithEffects) {
  // The effect row is in the IR for exactly this: a call that does
  // something cannot be removed just because its result is unused. The
  // callee here has no body, so inlining cannot make the question go away.
  Fixture f(R"(
fun Run() {
  Log()
}

offer fun Log() uses io
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Optimized("Run").find("call @Log") != std::string::npos);
}

WOLANG_TEST(PassTest, InlinesASmallCall) {
  // The rewrite needs no renumbering: the call instruction becomes the
  // parameter of the block control comes back to, so `%1` still names the
  // result and nothing that used it was touched.
  Fixture f(R"(
fun Double(n: int) -> int => n + n

fun Use(n: int) -> int => Double(n) + 1
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Lowered("Use").find("call @Double") != std::string::npos);
  WOLANG_EXPECT(f.Optimized("Use").find("call") == std::string::npos);
  WOLANG_EXPECT(f.Optimized("Use").find("add") != std::string::npos);
}

WOLANG_TEST(PassTest, LeavesRecursionAlone) {
  // A function that calls itself would inline forever.
  Fixture f(R"(
fun Countdown(n: int) -> int {
  if n == 0 {
    return 0
  }
  return Countdown(n - 1)
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Optimized("Countdown").find("call @Countdown") !=
                std::string::npos);
}

WOLANG_TEST(PassTest, PromotesASlotAndRemovesIt) {
  Fixture f(R"(
fun Promoted(n: int) -> int {
  var x = n + 1
  return x
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Lowered("Promoted").find("alloca") != std::string::npos);

  const std::string body = f.Optimized("Promoted");
  WOLANG_EXPECT(body.find("alloca") == std::string::npos);
  WOLANG_EXPECT(body.find("load") == std::string::npos);
  WOLANG_EXPECT(body.find("store") == std::string::npos);
  WOLANG_EXPECT(body.find("add %0") != std::string::npos);
}

WOLANG_TEST(PassTest, PromotesASlotWrittenOnSeveralPaths) {
  // A loop counter is written every iteration, so the value at the top of
  // the loop depends on which edge arrived -- which is exactly what a block
  // parameter is. Without this the counter makes a round trip through the
  // frame every iteration, and the loop runs at a third of its speed.
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
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Lowered("Sum").find("alloca") != std::string::npos);

  const std::string body = f.Optimized("Sum");
  WOLANG_EXPECT(body.find("alloca") == std::string::npos);
  WOLANG_EXPECT(body.find("load") == std::string::npos);
  WOLANG_EXPECT(body.find("store") == std::string::npos);
  // Two values go round the loop, so the header takes two parameters.
  WOLANG_EXPECT(body.find("loop.header") != std::string::npos);
  WOLANG_EXPECT(body.find("^1(") != std::string::npos);
}

WOLANG_TEST(PassTest, TurnsABranchOnAConstantIntoAJump) {
  Fixture f(R"(
fun Always() -> int {
  if true {
    return 1
  }
  return 2
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Optimized("Always");
  WOLANG_EXPECT(body.find("br ") == std::string::npos);
  // And then the jump itself goes: a block only one block reaches is part
  // of that block, so what is left is the one line that runs.
  WOLANG_EXPECT(body.find("jmp") == std::string::npos);
  // The arm that can no longer be reached is gone from the dump.
  WOLANG_EXPECT(body.find("if.else") == std::string::npos);
}

WOLANG_TEST(PassTest, MergesABlockOnlyOneBlockReaches) {
  // An early return leaves the else arm jumping straight to what follows
  // the `if`, and a block one block reaches is part of that block.
  Fixture f(R"(
fun Early(c: bool) -> int {
  if c {
    return 1
  }
  return 2
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Lowered("Early").find("if.join") != std::string::npos);
  WOLANG_EXPECT(f.Optimized("Early").find("if.join") == std::string::npos);
  WOLANG_EXPECT(f.Optimized("Early").find("jmp") == std::string::npos);
}

WOLANG_TEST(PassTest, MergingSubstitutesTheBlockParameters) {
  // The parameters of a merged block are replaced by the values the jump
  // carried. Without that, a block with parameters could never be merged --
  // and an inlined body always comes back through one.
  Fixture f(R"(
fun Double(n: int) -> int => n + n

fun Use() -> int => Double(21)
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Optimized("Use");
  WOLANG_EXPECT(body.find("call") == std::string::npos);
  WOLANG_EXPECT(body.find("jmp") == std::string::npos);
  // Folded, because after substitution both operands are the constant.
  WOLANG_EXPECT(body.find("42") != std::string::npos);
}

WOLANG_TEST(PassTest, ComputesARepeatedAddressOnce) {
  // Reading a field, adding to it and writing it back asks for the same
  // address twice. One of the two is enough.
  Fixture f(R"(
struct Point {
  var x: int = 0
  var y: int = 0
}

fun Bump(points: []Point) {
  var i = 0
  while i < points.len {
    points[i].x = points[i].x + 1
    i = i + 1
  }
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string lowered = f.Lowered("Bump");
  const std::string optimized = f.Optimized("Bump");
  const auto count = [](const std::string& body, std::string_view what) {
    size_t total = 0;
    for (size_t at = body.find(what); at != std::string::npos;
         at = body.find(what, at + 1)) {
      ++total;
    }
    return total;
  };
  WOLANG_EXPECT(count(lowered, "index.addr") >= 2);
  WOLANG_EXPECT_EQ(count(optimized, "index.addr"), size_t{1});
}

WOLANG_TEST(PassTest, MovesWhatDoesNotChangeOutOfTheLoop) {
  // The length of the slice and the base of its storage are the same every
  // iteration, and reading them is what the loop would otherwise spend a
  // third of itself on.
  Fixture f(R"(
fun Total(values: []int) -> int {
  var sum = 0
  var i = 0
  while i < values.len {
    sum = sum + values[i]
    i = i + 1
  }
  return sum
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string optimized = f.Optimized("Total");
  // The extracts that take the slice apart happen before the loop: the
  // block they end up in is the one with no label of its own.
  const size_t loop = optimized.find("// loop.header");
  WOLANG_EXPECT(loop != std::string::npos);
  WOLANG_EXPECT(optimized.find("extract") < loop);
  WOLANG_EXPECT(optimized.find("extract", loop) == std::string::npos);
}

WOLANG_TEST(PassTest, DropsTheBoundsCheckTheLoopAlreadyMade) {
  // `while i < bodies.len` is the same comparison the element read has to
  // make, so the second one is removed -- and the arithmetic around it is
  // out of the loop (D105).
  Fixture f(R"(
struct Body {
  var x: float = 0.0
  var y: float = 0.0
}

fun Sum(bodies: soa []Body) -> float {
  var total = 0.0
  var i = 0
  while i < bodies.len {
    total = total + bodies[i].x
    i = i + 1
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string optimized = f.Optimized("Sum");
  const size_t loop = optimized.find("// loop.header");
  WOLANG_EXPECT(loop != std::string::npos);
  WOLANG_EXPECT(optimized.find("index.check", loop) == std::string::npos);
  WOLANG_EXPECT(optimized.find("soa.addr") == std::string::npos);
}

WOLANG_TEST(PassTest, KeepsABoundsCheckNothingProved) {
  // The one part of an element address that is about *this* element. It
  // produces no value, so a pass that treats it as pure deletes it, and a
  // pass that treats it as loop invariant moves it somewhere it checks
  // nothing -- both of which this noticed. Here the loop counts one
  // thing and the index is another, so the check has to stay.
  Fixture f(R"(
struct Body {
  var x: float = 0.0
  var y: float = 0.0
}

fun Sum(bodies: soa []Body, at: int) -> float {
  var total = 0.0
  var i = 0
  while i < 4 {
    total = total + bodies[at].x
    i = i + 1
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string optimized = f.Optimized("Sum");
  WOLANG_EXPECT(optimized.find("index.check") != std::string::npos);
}

WOLANG_TEST(PassTest, EveryPassLeavesTheFunctionVerifiable) {
  // The exit criterion of the wave, as a test rather than a promise.
  Fixture f(R"(
enum Shape {
  Circle(radius: float)
  Point
}

struct Vec3 {
  var x: float
  var y: float
}

fun Mix(n: int, p: Vec3?, s: Shape) -> int {
  var total = 0
  var i = 0
  while i < n {
    if i > 2 && n < 100 {
      total = total + 1
    }
    i = i + 1
  }
  const area = match s {
    Circle(r) => 1
    Point => 0
  }
  const reach = p?.x ?? 0.0
  if const q = p {
    total = total + 1
  }
  return total + area
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
}

}  // namespace
}  // namespace wolang
