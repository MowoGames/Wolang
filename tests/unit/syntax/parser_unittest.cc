// Copyright 2026 The Wolang Authors. All rights reserved.

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
  Fixture fixture("let x = 1 + 2 * 3");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary +\n"
                   "  (int 1)\n"
                   "  (binary *\n"
                   "    (int 2)\n"
                   "    (int 3)))\n");
}

WOLANG_TEST(Parser, ComparisonBindsLooserThanArithmetic) {
  Fixture fixture("let x = a + 1 < b * 2");
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
  Fixture fixture("let x = a == 1 && b != 2 || c");
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
  Fixture fixture("let x = a - b - c");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary -\n"
                   "  (binary -\n"
                   "    (path a)\n"
                   "    (path b))\n"
                   "  (path c))\n");
}

WOLANG_TEST(Parser, NullCoalescingAndOptionalAccess) {
  Fixture fixture("let x = target?.name ?? \"none\"");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary ??\n"
                   "  (optional-field name\n"
                   "    (path target))\n"
                   "  (string\n"
                   "    (text \"none\")))\n");
}

WOLANG_TEST(Parser, UnaryOperators) {
  Fixture fixture("let x = -a + !b");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(binary +\n"
                   "  (unary -\n"
                   "    (path a))\n"
                   "  (unary !\n"
                   "    (path b)))\n");
}

WOLANG_TEST(Parser, RangesAndCasts) {
  Fixture fixture("let x = 0..10");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(range ..\n"
                   "  (from\n"
                   "    (int 0))\n"
                   "  (to\n"
                   "    (int 10)))\n");

  Fixture inclusive("let x = 0..=10");
  WOLANG_EXPECT(inclusive.InitTree().find("(range ..=") == 0);

  Fixture cast("let x = value as i64");
  WOLANG_EXPECT_EQ(cast.InitTree(),
                   "(cast\n"
                   "  (path value)\n"
                   "  (type i64))\n");
}

WOLANG_TEST(Parser, CallsIndexingAndMemberAccess) {
  Fixture fixture("let x = world.Find(a, b)[0].name");
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
  Fixture fixture("let x = a < b && c > d");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.InitTree().find("(binary &&") == 0);
}

WOLANG_TEST(Parser, ExplicitGenericCall) {
  Fixture fixture("let x = World.Spawn<Projectile>(origin)");
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
  Fixture fixture("let x = Make<Map<K, Array<V>>>(seed)");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.InitTree().find("(type Array\n") != std::string::npos);
  WOLANG_EXPECT(fixture.InitTree().find("(args\n    (path seed))") !=
                std::string::npos);
}

WOLANG_TEST(Parser, MethodChainsMayStartALine) {
  Fixture fixture(
      "let x = World.Overlap(a)\n"
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
  Fixture fixture("let x = Weapon { ammo = 30 }");
  WOLANG_EXPECT_EQ(fixture.InitTree(),
                   "(struct-literal\n"
                   "  (path Weapon)\n"
                   "  (args\n"
                   "    (assign =\n"
                   "      (path ammo)\n"
                   "      (int 30))))\n");

  Fixture call("let x = World.Spawn<P>(at) { speed = 40.0 }");
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
      "  if let t = target { Fire(t) } else if ready { Wait() } else { Idle() }\n"
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
  Fixture fixture("fun f() { await Wait(1s)\n  spawn Loop()\n  let v = read()? }");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(await") != std::string::npos);
  WOLANG_EXPECT(body.find("(spawn") != std::string::npos);
  WOLANG_EXPECT(body.find("(try") != std::string::npos);
  WOLANG_EXPECT(body.find("unit=s") != std::string::npos);
}

WOLANG_TEST(Parser, ClosuresWithAndWithoutParameters) {
  Fixture fixture("let x = map(|i| i * 2)");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.InitTree().find("(closure") != std::string::npos);

  Fixture empty("let x = run(|| 1)");
  WOLANG_EXPECT(empty.Codes().empty());
  WOLANG_EXPECT(empty.InitTree().find("(closure") != std::string::npos);
}

WOLANG_TEST(Parser, ArrayAndTupleLiterals) {
  Fixture array("let x = [1, 2, 3]");
  WOLANG_EXPECT_EQ(array.InitTree(),
                   "(array\n"
                   "  (int 1)\n"
                   "  (int 2)\n"
                   "  (int 3))\n");

  Fixture tuple("let x = (1, 2)");
  WOLANG_EXPECT(tuple.InitTree().find("(tuple\n") == 0);

  // A single parenthesized expression is just grouping.
  Fixture grouped("let x = (1 + 2) * 3");
  WOLANG_EXPECT(grouped.InitTree().find("(binary *") == 0);
}

WOLANG_TEST(Parser, UnsafeAndRegionBlocks) {
  Fixture fixture("fun f() { unsafe { p[0] = 1 }\n  region { Build() } }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.BodyTree().find("(unsafe") != std::string::npos);
  WOLANG_EXPECT(fixture.BodyTree().find("(region") != std::string::npos);
}

WOLANG_TEST(Parser, StringInterpolationBecomesExpressions) {
  Fixture fixture("let x = \"hp {hp * 2} / {max}\"");
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
  Fixture fixture("let x = \"value {1 +}\"");
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
  Fixture names("let with = 1\nlet without = 2\nlet state = 3");
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
      "  let a: int = 1\n"
      "  var b = 2\n"
      "  defer { Cleanup() }\n"
      "  if a { return }\n"
      "  while true { break }\n"
      "  return a\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string body = fixture.BodyTree();
  WOLANG_EXPECT(body.find("(let\n    (bind a)\n    (type int)") !=
                std::string::npos);
  WOLANG_EXPECT(body.find("(var\n") != std::string::npos);
  WOLANG_EXPECT(body.find("(defer") != std::string::npos);
  WOLANG_EXPECT(body.find("(break)") != std::string::npos);
  WOLANG_EXPECT(body.find("(return)") != std::string::npos);
}

WOLANG_TEST(Parser, SemicolonsAreAcceptedAsSeparators) {
  Fixture fixture("fun f() { let a = 1; let b = 2; }");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.BodyTree().find("(let\n    (bind b)") !=
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
      "offer async fun Closest<T: Actor + Damageable>(items: Span<T>,\n"
      "    from: Vec3 = origin) -> T? uses rand, io where T: Spawnable {\n"
      "  return null\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(fun Closest offer async") != std::string::npos);
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

WOLANG_TEST(Parser, TraitMethodsNeedNoBody) {
  Fixture fixture(
      "trait Damageable {\n"
      "  fun TakeDamage(amount: float, from: Actor?)\n"
      "  fun IsAlive() -> bool\n"
      "}");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(trait Damageable"));
  WOLANG_EXPECT(!fixture.TreeContains("(block"));
}

WOLANG_TEST(Parser, ClassWithMembers) {
  Fixture fixture(
      "/// A turret.\n"
      "@Spawnable\n"
      "class Turret : Actor {\n"
      "  @Edit @Range(1, 50) var range: float = 15.0\n"
      "  let muzzle = attach<SceneComponent>(\"Muzzle\")\n"
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
  Fixture trait_impl(
      "extend Barrel as Damageable { fun TakeDamage(a: float) { Explode() } }");
  WOLANG_EXPECT(trait_impl.Codes().empty());
  WOLANG_EXPECT(trait_impl.TreeContains("(as\n"));
  WOLANG_EXPECT(trait_impl.TreeContains("(type Damageable)"));
  WOLANG_EXPECT(trait_impl.TreeContains("(type Barrel)"));

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
      "fun Load(p: str) -> Level!\n"
      "fun Lookup(k: str) -> Pawn?!\n");
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

WOLANG_TEST(Parser, SliceAndArrayTypes) {
  Fixture fixture(
      "fun Sum(xs: [float]) -> float\n"
      "fun Fixed(xs: [float; 3])\n"
      "fun Nested(xs: [[int]; 2])\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(slice\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(array-type\n") != std::string::npos);
  WOLANG_EXPECT(tree.find("(length\n") != std::string::npos);
}

WOLANG_TEST(Parser, StorageLayoutBelongsToTheStorage) {
  Fixture fixture(
      "var burst: soa [Particle]\n"
      "var packed: aos [Particle]\n"
      "var one: Particle\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  const std::string tree = fixture.Tree();
  WOLANG_EXPECT(tree.find("(slice soa") != std::string::npos);
  WOLANG_EXPECT(tree.find("(slice aos") != std::string::npos);
  // The element type stays usable on its own.
  WOLANG_EXPECT(tree.find("(type Particle)") != std::string::npos);

  // `soa` is contextual, so it is still an ordinary name.
  Fixture name("let soa = 1\nlet aos = 2");
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
  Fixture fixture("fun f() { if let Point { x, y: inner } = p { Use(x) } }");
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
      "  let a = xs[2..]\n"
      "  let b = xs[..5]\n"
      "  let c = xs[..]\n"
      "  let d = xs[1..=3]\n"
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
  Fixture lower("let a = xs[2..]");
  Fixture upper("let a = xs[..2]");
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
      "    enter { await Play(\"Open\") }\n"
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
  Fixture fixture("comptime let Table = Build()\ncomptime fun Check() {}");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT(fixture.TreeContains("(let Table comptime"));
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
  Fixture fixture("fun f() {\n  let a = 1\n");
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
  Fixture fixture("fun f() {\n  let a = *\n  let b = 2\n}");
  WOLANG_EXPECT(fixture.HasCode("E0024"));
  WOLANG_EXPECT(fixture.BodyTree().find("(bind b)") != std::string::npos);
}

WOLANG_TEST(Parser, RequiresALineBreakBetweenStatements) {
  Fixture fixture("fun f() { let a = 1 let b = 2 }");
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
  std::string code = "let x = ";
  code.append(400, '(');
  code.append("1");
  code.append(400, ')');
  Fixture fixture(code);
  WOLANG_EXPECT(fixture.HasCode("E0041"));
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

}  // namespace
}  // namespace wolang
