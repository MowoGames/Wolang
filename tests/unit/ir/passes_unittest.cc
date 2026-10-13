// Copyright 2026 The Wolang Authors. All rights reserved.

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
  let _unused = n > 0
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
  let _unused = n + n
  return n
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Optimized("Kept").find("add") != std::string::npos);
}

WOLANG_TEST(PassTest, KeepsACallWithEffects) {
  // The effect row is in the IR for exactly this: a call that does something
  // cannot be removed just because its result is unused.
  Fixture f(R"(
fun Log() uses io {}

fun Run() {
  Log()
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Optimized("Run").find("call @Log") != std::string::npos);
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

WOLANG_TEST(PassTest, LeavesASlotWrittenOnSeveralPathsAlone) {
  // Promoting this one needs a block parameter on the join, which the pass
  // does not insert yet. Leaving it is correct; getting it wrong would not
  // be.
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
  WOLANG_EXPECT(f.Optimized("Sum").find("alloca") != std::string::npos);
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
  WOLANG_EXPECT(body.find("jmp ^1") != std::string::npos);
  // The arm that can no longer be reached is gone from the dump.
  WOLANG_EXPECT(body.find("if.else") == std::string::npos);
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
  let area = match s {
    Circle(r) => 1
    Point => 0
  }
  let reach = p?.x ?? 0.0
  if let q = p {
    total = total + 1
  }
  return total + area
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
}

}  // namespace
}  // namespace wolang
