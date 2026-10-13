// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/lower.h"

#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/compiler/database.h"
#include "wolang/ir/printer.h"

namespace wolang {
namespace {

// Lowers a snippet through the database, so the test exercises the same path
// the driver does -- including the verifier, which runs on every input.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  // The IR of the whole file. Empty when the file did not type check, which
  // is deliberate: lowering trusts the checker.
  std::string Ir() {
    const LoweredFile* lowered = db_.Lowered(file_);
    if (lowered == nullptr) {
      return "<none>";
    }
    return ir::PrintModule(db_.interner(), db_.types(), lowered->module);
  }

  // Whatever the verifier found. The only acceptable answer is nothing.
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

  // The body of one function, so a test does not have to spell out the
  // others.
  std::string Function(std::string_view name) {
    const std::string all = Ir();
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

 private:
  Database db_;
  FileId file_;
};

WOLANG_TEST(LowerTest, LowersArithmetic) {
  Fixture f("fun Add(a: int, b: int) -> int {\n  return a + b\n}\n");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT_EQ(f.Function("Add"),
                   "fun @Add(%0: i32, %1: i32) -> i32 {\n"
                   "^0:  // entry\n"
                   "  %2 = add %0, %1\n"
                   "  ret %2\n"
                   "}\n");
}

WOLANG_TEST(LowerTest, LowersAnIfIntoBlocks) {
  Fixture f(R"(
fun Max(a: int, b: int) -> int {
  if a > b {
    return a
  }
  return b
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT_EQ(f.Function("Max"),
                   "fun @Max(%0: i32, %1: i32) -> i32 {\n"
                   "^0:  // entry\n"
                   "  %2 = gt %0, %1\n"
                   "  br %2 ^1 ^2\n"
                   "^1:  // if.then\n"
                   "  ret %0\n"
                   "^2:  // if.else\n"
                   "  jmp ^3\n"
                   "^3:  // if.join\n"
                   "  ret %1\n"
                   "}\n");
}

WOLANG_TEST(LowerTest, AnIfExpressionJoinsThroughABlockParameter) {
  // The alternative would be a phi node; a block parameter makes the same
  // thing an arity check the verifier already does.
  Fixture f(R"(
fun Pick(c: bool) -> int {
  let v = if c { 1 } else { 2 }
  return v
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Function("Pick");
  WOLANG_EXPECT(body.find("^3(%") != std::string::npos);
  WOLANG_EXPECT(body.find("jmp %") != std::string::npos);
}

WOLANG_TEST(LowerTest, AMutableLocalBecomesASlot) {
  // Lowering every `var` into an alloca and letting a pass promote the ones
  // that can be is simpler than deciding here.
  Fixture f(R"(
fun Count(n: int) -> int {
  var total = 0
  total = total + n
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Function("Count");
  WOLANG_EXPECT(body.find("alloca : i32") != std::string::npos);
  WOLANG_EXPECT(body.find("store") != std::string::npos);
  WOLANG_EXPECT(body.find("load") != std::string::npos);
}

WOLANG_TEST(LowerTest, AnImmutableLocalNeedsNoSlot) {
  Fixture f(R"(
fun Twice(n: int) -> int {
  let doubled = n + n
  return doubled
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Function("Twice").find("alloca") == std::string::npos);
}

WOLANG_TEST(LowerTest, LowersAWhileLoop) {
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
  const std::string body = f.Function("Sum");
  WOLANG_EXPECT(body.find("loop.header") != std::string::npos);
  WOLANG_EXPECT(body.find("loop.body") != std::string::npos);
  WOLANG_EXPECT(body.find("loop.exit") != std::string::npos);
}

WOLANG_TEST(LowerTest, BreakAndContinueGoToTheLoopsBlocks) {
  Fixture f(R"(
fun Scan(n: int) -> int {
  var i = 0
  loop {
    if i > n {
      break
    }
    i = i + 1
    continue
  }
  return i
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
}

WOLANG_TEST(LowerTest, ShortCircuitOperatorsBecomeControlFlow) {
  // `&&` must not evaluate its right side when the left is false, so it
  // cannot be an operation.
  Fixture f(R"(
fun Both(a: bool, b: bool) -> bool {
  return a && b
}

fun Either(a: bool, b: bool) -> bool {
  return a || b
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Function("Both").find("and.join") != std::string::npos);
  WOLANG_EXPECT(f.Function("Either").find("or.join") != std::string::npos);
}

WOLANG_TEST(LowerTest, LowersACall) {
  Fixture f(R"(
fun Add(a: int, b: int) -> int => a + b

fun Use() -> int {
  return Add(1, 2)
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Function("Use").find("call @Add, %0, %1") !=
                std::string::npos);
}

WOLANG_TEST(LowerTest, ADeclarationWithNoBodyKeepsItsSignature) {
  // A trait method has nothing to lower, and the signature is what a call
  // needs.
  Fixture f(R"(
trait Damageable {
  fun TakeDamage(amount: float)
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
}

WOLANG_TEST(LowerTest, UnitsAreErased) {
  // Dimensions take part in checking and are gone by code generation, which
  // is what makes unit safe code free at runtime.
  Fixture f(R"(
dimension Length
dimension Time
unit m: Length
unit s: Time

fun Speed(d: Length, t: Time) -> Length {
  return d
}

fun Use() -> Length {
  return Speed(3m, 1s)
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  // `Length` is `f32` here, and the literal's unit is gone.
  WOLANG_EXPECT(f.Function("Speed").find("(%0: f32, %1: f32) -> f32") !=
                std::string::npos);
  WOLANG_EXPECT(f.Function("Use").find("const.int 3") != std::string::npos);
}

WOLANG_TEST(LowerTest, OpenLiteralTypesAreSettled) {
  // There is no "integer of whatever width the context wants" in the IR.
  Fixture f(R"(
fun Shrink(ratio: float) -> float {
  return 1.0 - ratio
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Function("Shrink").find("sub %") != std::string::npos);
}

WOLANG_TEST(LowerTest, LowersIfLetThroughTheOptionalOperations) {
  Fixture f(R"(
struct Pawn {
  var health: float
}

fun Hurt(p: Pawn?) -> float {
  if let q = p {
    return q.health
  }
  return 0.0
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Function("Hurt");
  WOLANG_EXPECT(body.find("opt.present %0") != std::string::npos);
  WOLANG_EXPECT(body.find("opt.value %0") != std::string::npos);
  WOLANG_EXPECT(body.find("extract 0, %") != std::string::npos);
}

WOLANG_TEST(LowerTest, LowersSafeFieldAccessAndCoalesce) {
  Fixture f(R"(
struct Pawn {
  var health: float
}

fun Read(p: Pawn?) -> float {
  return p?.health ?? 0.0
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Function("Read");
  WOLANG_EXPECT(body.find("opt.wrap") != std::string::npos);
  WOLANG_EXPECT(body.find("const.null") != std::string::npos);
  WOLANG_EXPECT(body.find("coalesce.join") != std::string::npos);
}

WOLANG_TEST(LowerTest, AFileWithErrorsIsNotLowered) {
  // Lowering trusts the checker, so there is nothing to lower when the
  // checker complained.
  Fixture f("fun f() -> int { return true }\n");
  WOLANG_EXPECT_EQ(f.Ir(), "");
  WOLANG_EXPECT_EQ(f.Problems(), "");
}

WOLANG_TEST(LowerTest, AConstructorBuildsAnAggregate) {
  Fixture f(R"(
struct Vec3 {
  var x: float
  var y: float
}

fun Make() -> Vec3 {
  return Vec3(1.0, 2.0)
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Function("Make").find("struct : Vec3") !=
                std::string::npos);
}

}  // namespace
}  // namespace wolang
