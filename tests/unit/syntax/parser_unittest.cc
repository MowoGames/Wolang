// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/syntax/parser.h"

#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast_printer.h"
#include "wolang/syntax/lexer.h"

namespace wolang {
namespace {

// Lexes and parses one snippet, exposing the printed tree and the diagnostic
// codes. Trees are compared as text: the printer format is the golden form,
// which keeps the expectations readable and the failures diffable.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = sources_.AddVirtualFile("<test>", std::string(code));
    tokens_ = Lex(sources_, file_, &interner_, &diagnostics_);
    ast_ = Parse(sources_, tokens_, &interner_, &diagnostics_);
  }

  const Ast& ast() const { return ast_; }

  std::string Tree() const { return PrintAst(&sources_, interner_, ast_); }

  // The tree of the first declaration's initializer, which keeps expression
  // expectations free of the surrounding `let`.
  std::string InitTree() const {
    const std::span<const ItemId> items = ast_.Get(ast_.root());
    if (items.empty()) {
      return "<no declarations>\n";
    }
    return PrintExpr(&sources_, interner_, ast_, ast_.Get(items.front()).init);
  }

  // The tree of the first declaration's body.
  std::string BodyTree() const {
    const std::span<const ItemId> items = ast_.Get(ast_.root());
    if (items.empty()) {
      return "<no declarations>\n";
    }
    return PrintExpr(&sources_, interner_, ast_, ast_.Get(items.front()).body);
  }

  size_t item_count() const { return ast_.Get(ast_.root()).size(); }

  std::string Codes() const {
    std::string result;
    for (const Diagnostic& diagnostic : diagnostics_.diagnostics()) {
      if (!result.empty()) {
        result.push_back(',');
      }
      result.append(diagnostic.code);
    }
    return result;
  }

  bool HasCode(std::string_view code) const {
    for (const Diagnostic& diagnostic : diagnostics_.diagnostics()) {
      if (diagnostic.code == code) {
        return true;
      }
    }
    return false;
  }

  bool TreeContains(std::string_view needle) const {
    return Tree().find(needle) != std::string::npos;
  }

 private:
  SourceManager sources_;
  StringInterner interner_;
  DiagnosticEngine diagnostics_;
  TokenStream tokens_;
  Ast ast_;
  FileId file_;
};

// ------------------------------------------------------------ expressions ---

WOLANG_TEST(Parser, MultiplicationBindsTighterThanAddition) {
  Fixture fixture("const x = 1 + 2 * 3");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary +\n"
                   "  (int 1)\n"
                   "  (binary *\n"
                   "    (int 2)\n"
                   "    (int 3)))\n");
}

WOLANG_TEST(Parser, ComparisonBindsLooserThanArithmetic) {
  Fixture fixture("const x = a + 1 < b * 2");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary <\n"
                   "  (binary +\n"
                   "    (path a)\n"
                   "    (int 1))\n"
                   "  (binary *\n"
                   "    (path b)\n"
                   "    (int 2)))\n");
}

WOLANG_TEST(Parser, LogicalOperatorsBindLooserThanComparison) {
  Fixture fixture("const x = a == 1 && b != 2 || c");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary ||\n"
                   "  (binary &&\n"
                   "    (binary ==\n"
                   "      (path a)\n"
                   "      (int 1))\n"
                   "    (binary !=\n"
                   "      (path b)\n"
                   "      (int 2)))\n"
                   "  (path c))\n");
}

WOLANG_TEST(Parser, AssignmentIsRightAssociative) {
  Fixture fixture("fun f() { a = b = c }");
  WOLANG_EXPECT(fixture.BodyTree().find(
                    "(assign =\n"
                    "      (path a)\n"
                    "      (assign =\n"
                    "        (path b)\n"
                    "        (path c)))") != std::string::npos);
}

WOLANG_TEST(Parser, SubtractionIsLeftAssociative) {
  Fixture fixture("const x = a - b - c");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary -\n"
                   "  (binary -\n"
                   "    (path a)\n"
                   "    (path b))\n"
                   "  (path c))\n");
}

WOLANG_TEST(Parser, NullCoalescingAndOptionalAccess) {
  Fixture fixture("const x = target?.name ?? \"none\"");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary ??\n"
                   "  (optional-field name\n"
                   "    (path target))\n"
                   "  (string\n"
                   "    (text \"none\")))\n");
}

WOLANG_TEST(Parser, UnaryOperators) {
  Fixture fixture("const x = -a + !b");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary +\n"
                   "  (unary -\n"
                   "    (path a))\n"
                   "  (unary !\n"
                   "    (path b)))\n");
}

WOLANG_TEST(Parser, RangesAndCasts) {
  Fixture fixture("const x = 0..10");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(range ..\n"
                   "  (from\n"
                   "    (int 0))\n"
                   "  (to\n"
                   "    (int 10)))\n");

  Fixture inclusive("const x = 0..=10");
  WOLANG_EXPECT(inclusive.InitTree().find("(range ..=") == 0);

  Fixture cast("const x = value as s64");
  WOLANG_EXPECT_EQ(cast.InitTree(),
                   "(cast\n"
                   "  (path value)\n"
                   "  (type s64))\n");
}

WOLANG_TEST(Parser, CallsIndexingAndMemberAccess) {
  Fixture fixture("const x = world.Find(a, b)[0].name");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(field name\n"
                   "  (index\n"
                   "    (call\n"
                   "      (field Find\n"
                   "        (path world))\n"
                   "      (args\n"
                   "        (path a)\n"
                   "        (path b)))\n"
                   "    (int 0)))\n");
}

WOLANG_TEST(Parser, ComparisonIsNotMistakenForGenericArguments) {
  Fixture fixture("const x = a < b && c > d");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.InitTree().find("(binary &&") == 0);
}

WOLANG_TEST(Parser, ShiftInsideAComparisonIsNotGenericArguments) {
  // Found by the generator (D231): the scan counted the `>>` as two
  // closers and saw `a<(b>>(c))` as a call with explicit arguments.
  Fixture fixture("const x = a < ((b >> (c & 15)) * d)");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.InitTree().find("(binary <") == 0);
}

WOLANG_TEST(Parser, ExplicitGenericCall) {
  Fixture fixture("const x = World.Spawn<Projectile>(origin)");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(call\n"
                   "  (field Spawn\n"
                   "    (path World))\n"
                   "  (generics\n"
                   "    (type Projectile))\n"
                   "  (args\n"
                   "    (path origin)))\n");
}

WOLANG_TEST(Parser, NestedGenericArgumentsSplitTheShiftToken) {
  Fixture fixture("const x = Make<Map<K, Array<V>>>(seed)");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.InitTree().find("(type Array\n") != std::string::npos);
  WOLANG_EXPECT(fixture.InitTree().find("(args\n    (path seed))") !=
                std::string::npos);
}

WOLANG_TEST(Parser, MethodChainsMayStartALine) {
  Fixture fixture(
      "const x = World.Overlap(a)\n"
      "    .Closest(b)\n"
      "    .name");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.item_count(), size_t{1});
  WOLANG_EXPECT(fixture.InitTree().find("(field name") == 0);
}

WOLANG_TEST(Parser, OperatorAtLineStartEndsTheExpression) {
  // Otherwise `-b` on its own line would silently join the previous line.
  Fixture fixture("fun f() {\n  a\n  -b\n}");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.BodyTree(),
                   "(block\n"
                   "  (expr\n"
                   "    (path a))\n"
                   "  (expr\n"
                   "    (unary -\n"
                   "      (path b))))\n");
}

WOLANG_TEST(Parser, StructLiteralAndInitializerBlock) {
  Fixture fixture("const x = Weapon { ammo = 30 }");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(struct-literal\n"
                   "  (path Weapon)\n"
                   "  (args\n"
                   "    (assign =\n"
                   "      (path ammo)\n"
                   "      (int 30))))\n");

  Fixture call("const x = World.Spawn<P>(at) { speed = 40.0 }");
  WOLANG_EXPECT(call.Codes().empty());
  WOLANG_EXPECT(call.InitTree().find("(struct-literal") == 0);
}

WOLANG_TEST(Parser, ConditionsDoNotStartAStructLiteral) {
  Fixture fixture("fun f() { if ready { Fire() } }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.BodyTree(),
                   "(block\n"
                   "  (expr\n"
                   "    (if\n"
                   "      (path ready)\n"
                   "      (block\n"
                   "        (expr\n"
                   "          (call\n"
                   "            (path Fire)))))))\n");
}

WOLANG_TEST(Parser, IfElseChainAndIfLet) {
  Fixture fixture(
      "fun f() {\n"
      "  if const t = target { Fire(t) } else if ready { Wait() } else { Idle() }\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(if\n      (bind t)") != std::string::npos);
  WOLANG_EXPECT(body.find("(else\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(path ready)") != std::string::npos);
}

WOLANG_TEST(Parser, MatchArmsAlternativesAndGuards) {
  Fixture fixture(
      "fun f() {\n"
      "  match state {\n"
      "    Idle => LookAround()\n"
      "    Patrol(route) => Follow(route)\n"
      "    Chase(t) | Attack(t, _) if ready => MoveTo(t)\n"
      "    _ => { Stop() }\n"
      "  }\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(match") != std::string::npos);
  WOLANG_EXPECT(body.find("(variant-pattern Patrol") != std::string::npos);
  WOLANG_EXPECT(body.find("(guard") != std::string::npos);
  WOLANG_EXPECT(body.find("(wildcard)") != std::string::npos);
  // Four arms, each printed once.
  size_t arms = 0;
  for (size_t at = body.find("(arm"); at != std::string::npos;
       at = body.find("(arm", at + 1)) {
    ++arms;
  }
  WOLANG_EXPECT_EQ(arms, size_t{4});
}

WOLANG_TEST(Parser, LoopForms) {
  Fixture fixture(
      "fun f() {\n"
      "  loop { Tick() }\n"
      "  while alive { Tick() }\n"
      "  for i in 0..10 { Tick() }\n"
      "  foreach p in particles { Move(p) }\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(loop") != std::string::npos);
  WOLANG_EXPECT(body.find("(while") != std::string::npos);
  WOLANG_EXPECT(body.find("(for\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(foreach\n") != std::string::npos);
}

WOLANG_TEST(Parser, AwaitSpawnAndTry) {
  Fixture fixture("fun f() { wait Wait(1s)\n  spawn Loop()\n  const v = read()? }");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(wait") != std::string::npos);
  WOLANG_EXPECT(body.find("(spawn") != std::string::npos);
  WOLANG_EXPECT(body.find("(try") != std::string::npos);
  WOLANG_EXPECT(body.find("unit=s") != std::string::npos);
}

WOLANG_TEST(Parser, ClosuresWithAndWithoutParameters) {
  Fixture fixture("const x = map(|i| i * 2)");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.InitTree().find("(closure") != std::string::npos);

  Fixture empty("const x = run(|| 1)");
  WOLANG_EXPECT(empty.Codes().empty());
  WOLANG_EXPECT(empty.InitTree().find("(closure") != std::string::npos);
}

WOLANG_TEST(Parser, ArrayAndTupleLiterals) {
  Fixture array("const x = [1, 2, 3]");
  WOLANG_EXPECT_EQ(array.InitTree(),
                   "(array\n"
                   "  (int 1)\n"
                   "  (int 2)\n"
                   "  (int 3))\n");

  Fixture tuple("const x = (1, 2)");
  WOLANG_EXPECT(tuple.InitTree().find("(tuple\n") == 0);

  // A single parenthesized expression is just grouping.
  Fixture grouped("const x = (1 + 2) * 3");
  WOLANG_EXPECT(grouped.InitTree().find("(binary *") == 0);
}

WOLANG_TEST(Parser, UnsafeAndRegionBlocks) {
  Fixture fixture("fun f() { unsafe { p[0] = 1 }\n  arena { Build() } }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.BodyTree().find("(unsafe") != std::string::npos);
  WOLANG_EXPECT(fixture.BodyTree().find("(arena") != std::string::npos);
}

WOLANG_TEST(Parser, StringInterpolationBecomesExpressions) {
  Fixture fixture("const x = \"hp {hp * 2} / {max}\"");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(string\n"
                   "  (text \"hp \")\n"
                   "  (binary *\n"
                   "    (path hp)\n"
                   "    (int 2))\n"
                   "  (text \" / \")\n"
                   "  (path max))\n");
}

WOLANG_TEST(Parser, InterpolationErrorsPointInsideTheLiteral) {
  Fixture fixture("const x = \"value {1 +}\"");
  WOLANG_EXPECT(fixture.HasCode("E0024"));
}

// ------------------------------------------------------------------ types ---

WOLANG_TEST(Parser, TypeForms) {
  Fixture fixture(
      "fun f(a: Actor?, b: Array<float>, c: fun(int) -> bool, d: *mut float,\n"
      "     e: &Transform, g: (int, bool), h: engine.math.Vec3) {}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(optional") != std::string::npos);
  WOLANG_EXPECT(tree.find("(type Array") != std::string::npos);
  WOLANG_EXPECT(tree.find("(fun-type") != std::string::npos);
  WOLANG_EXPECT(tree.find("(pointer mut") != std::string::npos);
  WOLANG_EXPECT(tree.find("(reference") != std::string::npos);
  WOLANG_EXPECT(tree.find("(tuple-type") != std::string::npos);
  WOLANG_EXPECT(tree.find("(type engine.math.Vec3)") != std::string::npos);
}

WOLANG_TEST(Parser, QueryModifiersAreContextual) {
  Fixture fixture(
      "system S(q: Query<mut Velocity, with Falling, without Dead>) {}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(mut\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(with\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(without\n") != std::string::npos);

  // The same words stay usable as ordinary names.
  Fixture names("const with = 1\nconst without = 2\nconst state = 3");
  WOLANG_EXPECT(names.Codes().empty());
  WOLANG_EXPECT_EQ(names.item_count(), size_t{3});
}

// -------------------------------------------------------------- patterns ----

WOLANG_TEST(Parser, PatternForms) {
  Fixture fixture(
      "fun f() {\n"
      "  match v {\n"
      "    _ => 1\n"
      "    -1 => 2\n"
      "    \"text\" => 3\n"
      "    (a, b) => 4\n"
      "    Shape.Circle(r) => 5\n"
      "    Point { x, y: inner } => 6\n"
      "    mut other => 7\n"
      "  }\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(wildcard)") != std::string::npos);
  WOLANG_EXPECT(body.find("(literal\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(tuple-pattern") != std::string::npos);
  WOLANG_EXPECT(body.find("(variant-pattern Shape.Circle") !=
                std::string::npos);
  WOLANG_EXPECT(body.find("(struct-pattern Point") != std::string::npos);
  WOLANG_EXPECT(body.find("(bind other mut)") != std::string::npos);
}

// ------------------------------------------------------------ statements ----

WOLANG_TEST(Parser, StatementForms) {
  Fixture fixture(
      "fun f() {\n"
      "  const a: int = 1\n"
      "  var b = 2\n"
      "  defer { Cleanup() }\n"
      "  if a { return }\n"
      "  while true { break }\n"
      "  return a\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(const\n    (bind a)\n    (type int)") !=
                std::string::npos);
  WOLANG_EXPECT(body.find("(var\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(defer") != std::string::npos);
  WOLANG_EXPECT(body.find("(break)") != std::string::npos);
  WOLANG_EXPECT(body.find("(return)") != std::string::npos);
}

WOLANG_TEST(Parser, SemicolonsAreAcceptedAsSeparators) {
  Fixture fixture("fun f() { const a = 1; const b = 2; }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.BodyTree().find("(const\n    (bind b)") !=
                std::string::npos);
}

WOLANG_TEST(Parser, NestedDeclarationsAreStatements) {
  Fixture fixture("fun f() { fun inner() -> int => 1\n  inner() }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.BodyTree().find("(item\n    (fun inner") !=
                std::string::npos);
}

// ----------------------------------------------------------------- items ----

WOLANG_TEST(Parser, Imports) {
  Fixture fixture("need engine.gameplay\nneed engine.math.{Vec3, Quat}");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.Tree(),
                   "(file\n"
                   "  (need engine.gameplay)\n"
                   "  (need engine.math\n"
                   "    (names Vec3 Quat)))\n");
}

WOLANG_TEST(Parser, FunctionSignature) {
  Fixture fixture(
      "offer task fun Closest<T: Actor + Damageable>(items: Span<T>,\n"
      "    from: Vec3 = origin) -> T? uses rand, io where T: Spawnable {\n"
      "  return null\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(fun Closest offer task") != std::string::npos);
  WOLANG_EXPECT(tree.find("(generic T") != std::string::npos);
  WOLANG_EXPECT(tree.find("(type Actor)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(type Damageable)") != std::string::npos);
  // The `where` bound is merged into the inline bounds.
  WOLANG_EXPECT(tree.find("(type Spawnable)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(default") != std::string::npos);
  WOLANG_EXPECT(tree.find("(returns") != std::string::npos);
  WOLANG_EXPECT(tree.find("(uses") != std::string::npos);
}

WOLANG_TEST(Parser, ExpressionBodiedFunction) {
  Fixture fixture("fun Lerp(a: float, b: float, t: float) -> float => a + t");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.BodyTree().find("(binary +") == 0);
}

WOLANG_TEST(Parser, AbilityMethodsNeedNoBody) {
  Fixture fixture(
      "ability Damageable {\n"
      "  fun TakeDamage(amount: float, from: Actor?)\n"
      "  fun IsAlive() -> bool\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(ability Damageable"));
  WOLANG_EXPECT(!fixture.TreeContains("(block"));
}

WOLANG_TEST(Parser, ClassWithMembers) {
  Fixture fixture(
      "/// A turret.\n"
      "@Spawnable\n"
      "class Turret : Actor {\n"
      "  @Edit @Range(1, 50) var range: float = 15.0\n"
      "  const muzzle = attach<SceneComponent>(\"Muzzle\")\n"
      "  derived var alive: bool => health > 0\n"
      "  signal Destroyed(by: Actor?)\n"
      "  override fun BeginPlay() { spawn Scan() }\n"
      "  on TakeDamage(amount: int) { health -= amount }\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(class Turret") != std::string::npos);
  WOLANG_EXPECT(tree.find("(attr Spawnable)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(extends\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(attr Range\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(var alive derived") != std::string::npos);
  WOLANG_EXPECT(tree.find("(derive") != std::string::npos);
  WOLANG_EXPECT(tree.find("(signal Destroyed") != std::string::npos);
  WOLANG_EXPECT(tree.find("(fun BeginPlay override") != std::string::npos);
  WOLANG_EXPECT(tree.find("(on TakeDamage") != std::string::npos);
}

WOLANG_TEST(Parser, DocCommentsAttachToDeclarations) {
  Fixture fixture("/// Fires once.\n/// Twice.\nfun Fire() {}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::span<const ItemId> items = fixture.ast().Get(
      fixture.ast().root());
  WOLANG_ASSERT_EQ(items.size(), size_t{1});
  const Item& item = fixture.ast().Get(items.front());
  WOLANG_EXPECT(item.doc.file.is_valid());
  // Both `///` lines, without the trailing newline.
  WOLANG_EXPECT_EQ(item.doc.begin, 0u);
  WOLANG_EXPECT_EQ(item.doc.end, 26u);
}

WOLANG_TEST(Parser, EnumVariants) {
  Fixture fixture(
      "enum AiState {\n"
      "  Idle\n"
      "  Patrol(route: PatrolRoute)\n"
      "  Attack(target: Pawn, combo: int)\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(variant Idle)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(variant Patrol") != std::string::npos);
  WOLANG_EXPECT(tree.find("(param combo") != std::string::npos);
}

WOLANG_TEST(Parser, ExtendBlocks) {
  Fixture inherent("extend Vec3 { fun Flat() -> Vec3 => self }");
  WOLANG_EXPECT(inherent.Codes().empty());
  WOLANG_EXPECT(inherent.TreeContains("(extend Vec3"));
  WOLANG_EXPECT(inherent.TreeContains("(target"));

  // The target comes first either way, so the form reads as a sentence.
  Fixture ability_impl(
      "extend Barrel as Damageable { fun TakeDamage(a: float) { Explode() } }");
  WOLANG_EXPECT(ability_impl.Codes().empty());
  WOLANG_EXPECT(ability_impl.TreeContains("(as\n"));
  WOLANG_EXPECT(ability_impl.TreeContains("(type Damageable)"));
  WOLANG_EXPECT(ability_impl.TreeContains("(type Barrel)"));

  // `impl` is reserved, so old habits produce a message rather than silence.
  Fixture old_spelling("impl Vec3 { }");
  WOLANG_EXPECT(old_spelling.HasCode("E0011"));
}

WOLANG_TEST(Parser, ErrorSets) {
  Fixture fixture("error NotFound, Corrupt, PermissionDenied\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.Tree(),
                   "(file\n"
                   "  (error NotFound\n"
                   "    (names NotFound Corrupt PermissionDenied)))\n");
}

WOLANG_TEST(Parser, FallibleAndOptionalTypesArePostfix) {
  Fixture fixture(
      "fun Find(id: int) -> Pawn?\n"
      "fun Load(p: text) -> Level!\n"
      "fun Lookup(k: text) -> Pawn?!\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(optional\n        (type Pawn))") !=
                std::string::npos);
  WOLANG_EXPECT(tree.find("(fallible\n        (type Level))") !=
                std::string::npos);
  // `Pawn?!` nests outward: fallible of optional of Pawn.
  WOLANG_EXPECT(tree.find("(fallible\n        (optional\n          (type Pawn)))") !=
                std::string::npos);
}

WOLANG_TEST(Parser, StatedErrorSets) {
  Fixture fixture(
      "fun A() -> int!Small
"
      "fun B() -> int!(Small, json.Malformed)
"
      "fun C() -> int?!(Small)
");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(type Small)"));
  WOLANG_EXPECT(fixture.TreeContains("(type json.Malformed)"));
}

WOLANG_TEST(Parser, ABangAtTheEndOfALineStatesNoErrors) {
  Fixture fixture(
      "fun A() -> int!
"
      "Small
");
  WOLANG_EXPECT(!fixture.TreeContains("(type Small)
        (type int)"));
  WOLANG_EXPECT(fixture.TreeContains("(fallible
"));
}

WOLANG_TEST(Parser, ErrorsCarryData) {
  Fixture fixture("error Plain, Bad(line: int, why: text)
");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(names Plain Bad)"));
  WOLANG_EXPECT(fixture.TreeContains("line"));
  WOLANG_EXPECT(fixture.TreeContains("why"));
}

WOLANG_TEST(Parser, AnEmptyStatedSetIsAnError) {
  Fixture fixture("fun A() -> int!()
");
  WOLANG_EXPECT(!fixture.Codes().empty());
}

WOLANG_TEST(Parser, SliceAndArrayTypes) {
  Fixture fixture(
      "fun Sum(xs: []float) -> float\n"
      "fun Fixed(xs: [3]float)\n"
      "fun Nested(xs: [2][]int)\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(slice\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(array-type\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(length\n") != std::string::npos);
}

WOLANG_TEST(Parser, StorageLayoutBelongsToTheStorage) {
  Fixture fixture(
      "var burst: soa []Particle\n"
      "var packed: aos []Particle\n"
      "var one: Particle\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(slice soa") != std::string::npos);
  WOLANG_EXPECT(tree.find("(slice aos") != std::string::npos);
  // The element type stays usable on its own.
  WOLANG_EXPECT(tree.find("(type Particle)") != std::string::npos);

  // `soa` is contextual, so it is still an ordinary name.
  Fixture name("const soa = 1\nconst aos = 2");
  WOLANG_EXPECT(name.Codes().empty());
  WOLANG_EXPECT_EQ(name.item_count(), size_t{2});
}

WOLANG_TEST(Parser, OptionalPatternsNeedNoSomeOrNone) {
  Fixture fixture(
      "fun f() {\n"
      "  match count {\n"
      "    n? => n\n"
      "    null => 0\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(present\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(bind n)") != std::string::npos);
  WOLANG_EXPECT(body.find("(literal\n") != std::string::npos);

  // Nested optionals and a wildcard form.
  Fixture nested("fun f() { match x { inner?? => inner, _? => 1, _ => 0 } }");
  WOLANG_EXPECT(nested.Codes().empty());
  const std::string nested_body = nested.BodyTree();
  WOLANG_EXPECT(nested_body.find("(present") != std::string::npos);
  WOLANG_EXPECT(nested_body.find("(bind inner)") != std::string::npos);
}

WOLANG_TEST(Parser, StructPatternsWorkInConditions) {
  Fixture fixture("fun f() { if const Point { x, y: inner } = p { Use(x) } }");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(struct-pattern Point") != std::string::npos);
  WOLANG_EXPECT(body.find("field=y") != std::string::npos);
  WOLANG_EXPECT(body.find("(bind inner field=y)") != std::string::npos);
}

WOLANG_TEST(Parser, LabelledLoops) {
  Fixture fixture(
      "fun f() {\n"
      "  outer: loop {\n"
      "    inner: for i in items {\n"
      "      if done { break outer }\n"
      "      continue inner\n"
      "    }\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(loop label=outer") != std::string::npos);
  WOLANG_EXPECT(body.find("(for label=inner") != std::string::npos);
  WOLANG_EXPECT(body.find("(break outer)") != std::string::npos);
  WOLANG_EXPECT(body.find("(continue inner)") != std::string::npos);
}

WOLANG_TEST(Parser, ReportsAnUnknownLoopLabel) {
  Fixture fixture("fun f() { loop { break nowhere } }");
  WOLANG_EXPECT(fixture.HasCode("E0044"));
}

WOLANG_TEST(Parser, ALabelDoesNotSwallowTheNextStatement) {
  Fixture fixture("fun f() { loop { break\n  Cleanup() } }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.BodyTree().find("(break)") != std::string::npos);
  WOLANG_EXPECT(fixture.BodyTree().find("(path Cleanup)") !=
                std::string::npos);
}

WOLANG_TEST(Parser, IncompleteRanges) {
  Fixture fixture(
      "fun f() {\n"
      "  const a = xs[2..]\n"
      "  const b = xs[..5]\n"
      "  const c = xs[..]\n"
      "  const d = xs[1..=3]\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  // Each bound is labelled, so `2..` and `..5` are distinguishable even
  // though both have exactly one child.
  WOLANG_EXPECT(body.find("(from\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(to\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(range ..)") != std::string::npos);
  WOLANG_EXPECT(body.find("(range ..=") != std::string::npos);

  // The printed forms of the two one-sided ranges differ.
  Fixture lower("const a = xs[2..]");
  Fixture upper("const a = xs[..2]");
  WOLANG_EXPECT_NE(lower.InitTree(), upper.InitTree());
}

WOLANG_TEST(Parser, ComponentsTagsAndSystems) {
  Fixture fixture(
      "component Velocity { var value: Vec3 }\n"
      "tag Falling\n"
      "system ApplyGravity(dt: float, q: Query<mut Velocity>) after Input,\n"
      "    before Integrate {\n"
      "  foreach v in q { v.value = dt }\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(component Velocity") != std::string::npos);
  WOLANG_EXPECT(tree.find("(tag Falling)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(after Input)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(before Integrate)") != std::string::npos);
}

WOLANG_TEST(Parser, DimensionsAndUnits) {
  Fixture fixture(
      "dimension Length, Time\n"
      "unit m: Length\n"
      "unit cm = 0.01m\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.Tree(),
                   "(file\n"
                   "  (dimension Length\n"
                   "    (names Length Time))\n"
                   "  (unit m\n"
                   "    (dimension Length))\n"
                   "  (unit cm\n"
                   "    (init\n"
                   "      (float 0.01 unit=m))))\n");
}

WOLANG_TEST(Parser, EffectsAndTypeAliases) {
  Fixture fixture("effect Telemetry\ntype Health = int");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.Tree(),
                   "(file\n"
                   "  (effect Telemetry)\n"
                   "  (type Health\n"
                   "    (of\n"
                   "      (type int))))\n");
}

WOLANG_TEST(Parser, StateMachines) {
  Fixture fixture(
      "machine Door {\n"
      "  state Closed {\n"
      "    on Interact -> Opening\n"
      "  }\n"
      "  state Opening {\n"
      "    enter { wait Play(\"Open\") }\n"
      "    exit { Done() }\n"
      "    always -> Open\n"
      "  }\n"
      "  state Open {\n"
      "    on Interact after 3s -> Closing\n"
      "  }\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(machine Door") != std::string::npos);
  WOLANG_EXPECT(tree.find("(transition Interact -> Opening)") !=
                std::string::npos);
  WOLANG_EXPECT(tree.find("(transition always -> Open)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(enter") != std::string::npos);
  WOLANG_EXPECT(tree.find("(exit") != std::string::npos);
  WOLANG_EXPECT(tree.find("(after\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("unit=s") != std::string::npos);
}

WOLANG_TEST(Parser, ExternFunctions) {
  Fixture fixture(
      "extern \"C\" fun FastNoise(x: float) -> float = \"wolang_noise\"");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(fun FastNoise extern") != std::string::npos);
  WOLANG_EXPECT(tree.find("(symbol wolang_noise)") != std::string::npos);
  WOLANG_EXPECT(tree.find("(abi C)") != std::string::npos);
}

WOLANG_TEST(Parser, ComptimeDeclarations) {
  Fixture fixture("comptime const Table = Build()\ncomptime fun Check() {}");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(const Table comptime"));
  WOLANG_EXPECT(fixture.TreeContains("(fun Check comptime"));
}

WOLANG_TEST(Parser, LayoutAttributeOnStruct) {
  Fixture fixture(
      "@layout(soa)\n"
      "struct Particle { var position: Vec3\n  var life: float }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(attr layout\n"));
  WOLANG_EXPECT(fixture.TreeContains("(path soa)"));
}

// ------------------------------------------------------------- recovery -----

WOLANG_TEST(Parser, ReportsGarbageAtTopLevelAndKeepsGoing) {
  Fixture fixture("42\nfun f() {}\n");
  WOLANG_EXPECT(fixture.HasCode("E0020"));
  // The declaration after the garbage still lands in the tree.
  WOLANG_EXPECT(fixture.TreeContains("(fun f"));
}

WOLANG_TEST(Parser, DoesNotRepeatWhatTheLexerAlreadyReported) {
  // The lexer reports the stray characters; the parser must not add a second
  // complaint about the same place.
  Fixture fixture("$$$\nfun f() {}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0005");
  WOLANG_EXPECT(fixture.TreeContains("(fun f"));
}

WOLANG_TEST(Parser, ReportsUnmatchedBrace) {
  Fixture fixture("fun f() {\n  const a = 1\n");
  WOLANG_EXPECT(fixture.HasCode("E0026"));
}

WOLANG_TEST(Parser, RecoversInsideAClassBody) {
  Fixture fixture(
      "class C {\n"
      "  @@@\n"
      "  fun good() {}\n"
      "}");
  WOLANG_EXPECT(!fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(fun good"));
}

WOLANG_TEST(Parser, RecoversFromABadStatement) {
  Fixture fixture("fun f() {\n  const a = *\n  const b = 2\n}");
  WOLANG_EXPECT(fixture.HasCode("E0024"));
  WOLANG_EXPECT(fixture.BodyTree().find("(bind b)") != std::string::npos);
}

WOLANG_TEST(Parser, RequiresALineBreakBetweenStatements) {
  Fixture fixture("fun f() { const a = 1 const b = 2 }");
  WOLANG_EXPECT(fixture.HasCode("E0040"));
}

WOLANG_TEST(Parser, ReportsMissingFunctionBody) {
  Fixture fixture("fun f() ???");
  WOLANG_EXPECT(!fixture.Codes().empty());
}

WOLANG_TEST(Parser, ReportsDuplicateModifiers) {
  Fixture fixture("offer offer fun f() {}");
  WOLANG_EXPECT(fixture.HasCode("E0029"));
  WOLANG_EXPECT(fixture.TreeContains("(fun f offer"));
}

WOLANG_TEST(Parser, ReportsUnknownWhereParameter) {
  Fixture fixture("fun f<T>() where U: Actor {}");
  WOLANG_EXPECT(fixture.HasCode("E0042"));
}

WOLANG_TEST(Parser, ReportsBadMatchArm) {
  Fixture fixture("fun f() { match v { => 1 } }");
  WOLANG_EXPECT(fixture.HasCode("E0034"));
}

WOLANG_TEST(Parser, ReportsBadStateAndTransition) {
  Fixture states("machine M { fun oops() {} }");
  WOLANG_EXPECT(states.HasCode("E0035"));

  Fixture transition("machine M { state S { oops } }");
  WOLANG_EXPECT(transition.HasCode("E0036"));
}

WOLANG_TEST(Parser, ReportsUnitWithoutDefinition) {
  Fixture fixture("unit m");
  WOLANG_EXPECT(fixture.HasCode("E0037"));
}

WOLANG_TEST(Parser, ReportsMissingTypeAndPattern) {
  Fixture type("fun f(a: ) {}");
  WOLANG_EXPECT(type.HasCode("E0023"));

  Fixture pattern("fun f() { for in items {} }");
  WOLANG_EXPECT(pattern.HasCode("E0025"));
}

WOLANG_TEST(Parser, ReportsDeeplyNestedExpressions) {
  std::string code = "const x = ";
  code.append(400, '(');
  code.append("1");
  code.append(400, ')');
  Fixture fixture(code);
  WOLANG_EXPECT(fixture.HasCode("E0041"));
}

// The chains below are built by loops, not recursion, so the paren limit never
// sees them; the tree height limit does (D225).
WOLANG_TEST(Parser, ReportsDeepBinaryChain) {
  std::string code = "const x = 1";
  for (int i = 0; i < 100000; ++i) {
    code += "+1";
  }
  Fixture fixture(code);
  WOLANG_EXPECT(fixture.HasCode("E0041"));
}

WOLANG_TEST(Parser, ReportsDeepPrefixChain) {
  std::string code = "const x = ";
  code.append(100000, '-');
  code += "1";
  Fixture fixture(code);
  WOLANG_EXPECT(fixture.HasCode("E0041"));
}

WOLANG_TEST(Parser, ReportsDeepMethodChain) {
  std::string code = "const x = a";
  for (int i = 0; i < 100000; ++i) {
    code += ".F()";
  }
  Fixture fixture(code);
  WOLANG_EXPECT(fixture.HasCode("E0041"));
}

WOLANG_TEST(Parser, ReportsDeepElseIfChain) {
  std::string code = "fun F() {\n if a { }";
  for (int i = 0; i < 5000; ++i) {
    code += " else if a { }";
  }
  code += "\n}\n";
  Fixture fixture(code);
  WOLANG_EXPECT(fixture.HasCode("E0041"));
}

WOLANG_TEST(Parser, AcceptsModerateBinaryChain) {
  std::string code = "const x = 1";
  for (int i = 0; i < 300; ++i) {
    code += "+1";
  }
  Fixture fixture(code);
  WOLANG_EXPECT(!fixture.HasCode("E0041"));
}

WOLANG_TEST(Parser, EmptyFileHasNoDeclarations) {
  Fixture fixture("");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.item_count(), size_t{0});
  WOLANG_EXPECT_EQ(fixture.Tree(), "(file)\n");
}

WOLANG_TEST(Parser, CommentsOnlyFileHasNoDeclarations) {
  Fixture fixture("// nothing here\n// still nothing\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.item_count(), size_t{0});
}

// ----------------------------------------------------------- whole files ----

WOLANG_TEST(Parser, ParsesTheBundledExamples) {
  for (std::string_view name : {"examples/turret.wo", "examples/hello.wo"}) {
    SourceManager sources;
    StringInterner interner;
    DiagnosticEngine diagnostics;

    std::string path = WOLANG_PROJECT_DIR;
    path.push_back('/');
    path.append(name);

    std::string error;
    const FileId file = sources.LoadFile(path, &error);
    WOLANG_ASSERT(file.is_valid());

    const TokenStream tokens = Lex(sources, file, &interner, &diagnostics);
    const Ast ast = Parse(sources, tokens, &interner, &diagnostics);

    if (!diagnostics.diagnostics().empty()) {
      WOLANG_FAIL(std::string(name) + " reported " +
                  diagnostics.diagnostics().front().code + ": " +
                  diagnostics.diagnostics().front().message);
    }
    WOLANG_EXPECT_GT(ast.Get(ast.root()).size(), size_t{0});
  }
}


WOLANG_TEST(Parser, EffectListAcceptsTheUnsafeKeyword) {
  // `unsafe` is a keyword everywhere else, but it is also a built-in effect.
  Fixture fixture("fun f() uses io, unsafe {}");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.Tree(),
                   "(file\n"
                   "  (fun f\n"
                   "    (uses\n"
                   "      (type io)\n"
                   "      (type unsafe))\n"
                   "    (block)))\n");
}

WOLANG_TEST(Parser, AKeywordMayBeALocalAName) {
  // A keyword that cannot begin an expression is a name wherever a name is
  // what the grammar wants: a binding, a parameter, or the middle of an
  // expression. `after` and `before` order systems and nothing else, so a
  // variable called one of them reads the way it was written.
  //
  // It still has to continue the line it is on. Statements are separated
  // by line breaks here, so a keyword at the start of one is the next
  // statement -- and without that rule a half-written expression swallowed
  // the declaration after it.
  const std::string source = R"(
struct Thing {
  var tag: int = 0
  var after: int = 2
}

fun Use(t: Thing, tag: int) -> int {
  const after = t.after
  const before = t.tag
  return after + before + tag
}
)";
  Fixture f(source);
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(Parser, AKeywordMayBeAName) {
  // Where only a name can appear -- after a `.`, or as what a declaration
  // calls itself -- a keyword is a name. A language whose keywords are off
  // limits as field names makes its users rename their data, and `tag`,
  // `state` and `task` are words gameplay code wants.
  const std::string source = R"(
need engine.task

struct Thing {
  var tag: int = 0
  var state: int = 1

  fun Total() -> int => self.tag + self.state
}

fun Use(t: Thing) -> int {
  return t.tag + t.Total()
}
)";
  Fixture f(source);
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(ParserTest, ANestedGenericIsNotTwoArguments) {
  // `List<List<T>>` ends on one token that closes two lists, and the
  // comma after it belongs to the parameter list. Read the other way
  // round -- which is what happened -- the generic swallowed the comma
  // and the function lost a parameter (D163).
  const std::string source = R"(
struct Pair<A, B> { var a: A var b: B }

fun Rows(rows: Pair<Pair<int, int>, int>, count: int) -> int {
  return count
}

fun Deeper(x: Pair<int, Pair<int, Pair<int, int>>>, y: int) -> int {
  return y
}

fun Returned(a: int) -> Pair<Pair<int, int>, int>
)";
  Fixture f(source);
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

}  // namespace
}  // namespace wolang
