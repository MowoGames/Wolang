// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/lower.h"

#include <string>
#include <string_view>

#include "harness/temp_tree.h"
#include "harness/test.h"
#include "wolang/compiler/database.h"
#include "wolang/ir/printer.h"
#include "wolang/compiler/workspace.h"

namespace wolang {
namespace {

// Lowers a snippet through the database, so the test exercises the same path
// the driver does -- including the verifier, which runs on every input.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  // The IR as lowering left it, before any pass ran. What a lowering test
  // is about is what lowering produced; the passes have their own suite.
  std::string Ir() {
    const LoweredFile* lowered = db_.Lowered(file_);
    if (lowered == nullptr) {
      return "<none>";
    }
    return ir::PrintModule(db_.interner(), db_.types(),
                           lowered->unoptimized);
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

  const LoweredFile* Lowered() { return db_.Lowered(file_); }
  Symbol Interned(std::string_view name) {
    return db_.interner().Intern(name);
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
                   "fun @Add(%0: s64, %1: s64) -> s64 {\n"
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
                   "fun @Max(%0: s64, %1: s64) -> s64 {\n"
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
  const v = if c { 1 } else { 2 }
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
  WOLANG_EXPECT(body.find("alloca : s64") != std::string::npos);
  WOLANG_EXPECT(body.find("store") != std::string::npos);
  WOLANG_EXPECT(body.find("load") != std::string::npos);
}

WOLANG_TEST(LowerTest, AnImmutableLocalNeedsNoSlot) {
  Fixture f(R"(
fun Twice(n: int) -> int {
  const doubled = n + n
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
  // An ability method has nothing to lower, and the signature is what a call
  // needs.
  Fixture f(R"(
ability Damageable {
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
  if const q = p {
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


WOLANG_TEST(LowerTest, LowersAMatchIntoTestedArms) {
  Fixture f(R"(
fun Classify(n: int) -> int {
  return match n {
    0 => 10
    1 => 20
    _ => 30
  }
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Function("Classify");
  WOLANG_EXPECT(body.find("match.join") != std::string::npos);
  WOLANG_EXPECT(body.find("match.test") != std::string::npos);
  // The catch-all needs no test.
  WOLANG_EXPECT(body.find("match.dead") == std::string::npos);
}

WOLANG_TEST(LowerTest, AMatchWithNoCatchAllEndsUnreachable) {
  // The checker already proved the arms cover every value, so failing the
  // last test cannot happen -- and saying so is better than inventing a
  // branch to nowhere.
  Fixture f(R"(
fun Flag(b: bool) -> int {
  return match b {
    true => 1
    false => 0
  }
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Function("Flag").find("unreachable") != std::string::npos);
}

WOLANG_TEST(LowerTest, AMatchOnAnOptionalTestsPresence) {
  Fixture f(R"(
fun Read(p: int?) -> int {
  return match p {
    x? => x
    null => 0
  }
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Function("Read");
  WOLANG_EXPECT(body.find("opt.present") != std::string::npos);
  WOLANG_EXPECT(body.find("opt.value") != std::string::npos);
  WOLANG_EXPECT(body.find("lnot") != std::string::npos);
}

WOLANG_TEST(LowerTest, AMatchOnAnEnumTestsTheTag) {
  Fixture f(R"(
enum Shape {
  Circle(radius: float)
  Point
}

fun Area(s: Shape) -> float {
  return match s {
    Circle(r) => 1.0
    Point => 0.0
  }
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT(f.Function("Area").find("extract 0, %0") !=
                std::string::npos);
}

WOLANG_TEST(LowerTest, AGuardedArmTestsThePatternAndTheGuard) {
  Fixture f(R"(
fun Pick(n: int) -> int {
  return match n {
    0 if n > 0 => 1
    _ => 2
  }
}
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const std::string body = f.Function("Pick");
  WOLANG_EXPECT(body.find("and %") != std::string::npos);
}

WOLANG_TEST(LowerTest, EmitsOneSpecializationPerInstantiation) {
  // A generic function is a recipe, not a function: it has no code until
  // something instantiates it. Before monomorphization the IR carried `T`
  // and the verifier rightly rejected every call to it.
  Fixture f(R"(
fun Identity<T>(value: T) -> T {
  return value
}

fun UseInt() -> int => Identity(1) + Identity(2)

fun UseFloat() -> float => Identity(1.5)
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  // Two call sites with the same argument share one copy; a different
  // argument gets its own.
  WOLANG_EXPECT_EQ(f.Function("Identity<s64>"),
                   "fun @Identity<s64>(%0: s64) -> s64 {\n"
                   "^0:  // entry\n"
                   "  ret %0\n"
                   "}\n");
  WOLANG_EXPECT_EQ(f.Function("Identity<f32>"),
                   "fun @Identity<f32>(%0: f32) -> f32 {\n"
                   "^0:  // entry\n"
                   "  ret %0\n"
                   "}\n");
  // The generic itself is not in the IR at all.
  WOLANG_EXPECT_EQ(f.Function("Identity"), "<not found>");
}

WOLANG_TEST(LowerTest, SpecializesThroughAGenericCaller) {
  // The inner call's type argument is the outer function's type parameter,
  // so it only becomes concrete once the outer one is instantiated.
  Fixture f(R"(
fun Identity<T>(value: T) -> T => value

fun Twice<T>(value: T) -> T => Identity(Identity(value))

fun Use() -> int => Twice(3)
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  WOLANG_EXPECT_EQ(f.Function("Twice<s64>"),
                   "fun @Twice<s64>(%0: s64) -> s64 {\n"
                   "^0:  // entry\n"
                   "  %1 = call @Identity<s64>, %0\n"
                   "  %2 = call @Identity<s64>, %1\n"
                   "  ret %2\n"
                   "}\n");
}

WOLANG_TEST(LowerTest, AnUninferredEffectRowReadsAsUnknown) {
  // An empty row is an assertion -- "nothing observable" (D38) -- so a
  // function whose row was never inferred must not read as one that does
  // nothing. Effects are inferred per module, and a file outside a package
  // has no module, so this is what a standalone file gets.
  Fixture f(R"(
fun Quiet() -> int => 1
)");
  WOLANG_EXPECT_EQ(f.Problems(), "");
  const LoweredFile* lowered = f.Lowered();
  WOLANG_EXPECT(lowered != nullptr);
  if (lowered != nullptr) {
    const ir::Function* quiet = lowered->module.Find(f.Interned("Quiet"));
    WOLANG_EXPECT(quiet != nullptr);
    if (quiet != nullptr) {
      WOLANG_EXPECT(!quiet->effects_known);
      WOLANG_EXPECT(quiet->effects.empty());
    }
  }
}

WOLANG_TEST(LowerTest, CarriesTheInferredEffectRowIntoTheIr) {
  // Wave 6 decided effect rows belong in the IR; this is what makes the
  // stored row an answer rather than a default nobody filled in. The rows
  // come from the module-wide fixpoint, so this needs a real package.
  testing::TempTree tree("lower-effects");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\nversion = \"1\"\n");
  tree.Write("src/main.wo", R"(
fun Quiet() -> int => 1

fun Loud() uses io {
  Shout()
}

offer fun Shout() uses io
)");
  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));

  for (size_t i = 0; i < db.file_count(); ++i) {
    const FileId file{static_cast<uint32_t>(i)};
    const LoweredFile* lowered = db.Lowered(file);
    if (lowered == nullptr ||
        lowered->module.Find(db.interner().Intern("Quiet")) == nullptr) {
      continue;
    }
    const ir::Function* quiet =
        lowered->module.Find(db.interner().Intern("Quiet"));
    const ir::Function* loud =
        lowered->module.Find(db.interner().Intern("Loud"));
    WOLANG_EXPECT(quiet != nullptr && loud != nullptr);
    if (quiet == nullptr || loud == nullptr) {
      return;
    }
    // Known and empty is an assertion: this function does nothing
    // observable.
    WOLANG_EXPECT(quiet->effects_known);
    WOLANG_EXPECT(quiet->effects.empty());
    // Inferred, not declared: `Loud` says `uses io` and the row reached the
    // IR through the fixpoint either way.
    WOLANG_EXPECT(loud->effects_known);
    WOLANG_EXPECT(loud->effects.Contains(BuiltinEffect::kIo));
    return;
  }
  WOLANG_EXPECT(false);
}

// Polymorphic recursion asks for a new copy with a bigger type each time; it
// has to stop with E0140 instead of lowering forever (D225).
std::string LoweringCodes(std::string_view code) {
  Database db;
  const FileId file = db.AddFile("<test>", std::string(code));
  const LoweredFile* lowered = db.Lowered(file);
  std::string codes;
  if (lowered != nullptr) {
    for (const Diagnostic& diagnostic : lowered->diagnostics) {
      codes.append(diagnostic.code);
    }
  }
  return codes;
}

WOLANG_TEST(Lower, PolymorphicRecursionOnAFunctionIsE0140) {
  WOLANG_EXPECT_EQ(LoweringCodes(R"(
fun F<T>(x: T, n: int) -> int {
  if n == 0 {
    return 0
  }
  return F([x], n - 1)
}
fun main() -> int {
  return F(1, 3)
}
)"), "E0140");
}

WOLANG_TEST(Lower, PolymorphicRecursionOnAMethodIsE0140) {
  WOLANG_EXPECT_EQ(LoweringCodes(R"(
extend<T> S<T> {
  fun M() -> int => S<S<T>>(1).M()
}
struct S<T> {
  var x: int
}
fun main() -> int {
  return S<int>(1).M()
}
)"), "E0140");
}

WOLANG_TEST(Lower, BoundedGenericRecursionIsNotE0140) {
  WOLANG_EXPECT_EQ(LoweringCodes(R"(
fun F<T>(x: T, n: int) -> int {
  if n == 0 {
    return 0
  }
  return F(x, n - 1)
}
fun main() -> int {
  return F(1, 3)
}
)"), "");
}

}  // namespace
}  // namespace wolang
