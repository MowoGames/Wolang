// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/format/formatter.h"

#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast_printer.h"
#include "wolang/syntax/lexer.h"
#include "wolang/syntax/parser.h"

namespace wolang {
namespace {

// One parse of one snippet, plus its formatted form and its printed tree.
class Pass {
 public:
  explicit Pass(std::string_view code, const FormatOptions& options = {}) {
    file_ = sources_.AddVirtualFile("<test>", std::string(code));
    tokens_ = Lex(sources_, file_, &interner_, &diagnostics_);
    ast_ = Parse(sources_, tokens_, &interner_, &diagnostics_);
    formatted_ = Format(sources_, interner_, tokens_, ast_, options);
  }

  const std::string& formatted() const { return formatted_; }
  std::string Tree() const { return PrintAst(nullptr, interner_, ast_); }
  bool HasErrors() const { return diagnostics_.has_errors(); }

 private:
  SourceManager sources_;
  StringInterner interner_;
  DiagnosticEngine diagnostics_;
  TokenStream tokens_;
  Ast ast_;
  FileId file_;
  std::string formatted_;
};

// Formats |code|, then checks the two properties that matter: formatting is
// idempotent, and it preserves the tree. The second is the real correctness
// statement -- a formatter that loses a name or reorders an operand would
// still be idempotent.
struct FormatResult {
  std::string output;
  bool idempotent = false;
  bool preserves_tree = false;
  bool second_pass_parses = false;
};

FormatResult Check(std::string_view code) {
  Pass first(code);
  FormatResult result;
  result.output = first.formatted();

  Pass second(result.output);
  result.second_pass_parses = !second.HasErrors();
  result.idempotent = second.formatted() == first.formatted();
  result.preserves_tree = second.Tree() == first.Tree();
  return result;
}

void ExpectStable(std::string_view code, std::string_view file, int line) {
  const FormatResult result = Check(code);
  if (!result.second_pass_parses) {
    ::wolang::test::ReportFailure(file, line,
                                  "formatted output does not parse:\n" +
                                      result.output);
    return;
  }
  if (!result.idempotent) {
    ::wolang::test::ReportFailure(
        file, line, "formatting is not idempotent:\n" + result.output);
  }
  if (!result.preserves_tree) {
    ::wolang::test::ReportFailure(
        file, line, "formatting changed the tree:\n" + result.output);
  }
}

#define WOLANG_EXPECT_STABLE(code) ExpectStable((code), __FILE__, __LINE__)

// ----------------------------------------------------------------- output ---

WOLANG_TEST(Formatter, NormalizesSpacingAndIndentation) {
  Pass pass("fn    f(a:int,b:float)->bool{return a>0}");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "fn f(a: int, b: float) -> bool {\n"
                   "  return a > 0\n"
                   "}\n");
}

WOLANG_TEST(Formatter, DropsSemicolonsAndExtraBlankLines) {
  Pass pass("fn f() {\n  let a = 1;\n\n\n  let b = 2;\n}\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "fn f() {\n"
                   "  let a = 1\n"
                   "\n"
                   "  let b = 2\n"
                   "}\n");
}

WOLANG_TEST(Formatter, WritesAnEmptyBodyOnOneLine) {
  Pass pass("fn f() {\n}\nclass C {\n}\n");
  WOLANG_EXPECT_EQ(pass.formatted(), "fn f() {}\nclass C {}\n");
}

WOLANG_TEST(Formatter, KeepsFloatLiteralsFloating) {
  // `15.0` must not come back as `15`, which would re-read as an integer.
  Pass pass("let a = 15.0\nlet b = 0.25s\nlet c = 1e10\nlet d = 200ms\n");
  // The shortest form that reads back as the same float wins, so an exponent
  // survives as an exponent rather than being expanded.
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "let a = 15.0\n"
                   "let b = 0.25s\n"
                   "let c = 1e+10\n"
                   "let d = 200ms\n");
  WOLANG_EXPECT_STABLE("let a = 15.0\nlet b = 0.25s\n");
}

WOLANG_TEST(Formatter, RestoresNeededParentheses) {
  // Grouping parentheses are not in the tree, so the formatter has to derive
  // them from precedence.
  Pass pass("let a = (1 + 2) * 3\n");
  WOLANG_EXPECT_EQ(pass.formatted(), "let a = (1 + 2) * 3\n");

  Pass kept("let a = 1 + 2 * 3\n");
  WOLANG_EXPECT_EQ(kept.formatted(), "let a = 1 + 2 * 3\n");

  Pass dropped("let a = 1 + (2 * 3)\n");
  WOLANG_EXPECT_EQ(dropped.formatted(), "let a = 1 + 2 * 3\n");

  Pass unary("let a = (-b).abs()\n");
  WOLANG_EXPECT_EQ(unary.formatted(), "let a = (-b).abs()\n");

  Pass right("let a = x = y = z\n");
  WOLANG_EXPECT_EQ(right.formatted(), "let a = x = y = z\n");

  Pass left("let a = (x - y) - z\n");
  WOLANG_EXPECT_EQ(left.formatted(), "let a = x - y - z\n");

  Pass needed("let a = x - (y - z)\n");
  WOLANG_EXPECT_EQ(needed.formatted(), "let a = x - (y - z)\n");
}

WOLANG_TEST(Formatter, BreaksLongArgumentLists) {
  Pass pass(
      "fn f() {\n"
      "  Spawn(a_long_argument, another_long_argument, "
      "a_third_long_argument, and_a_fourth_one)\n"
      "}\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "fn f() {\n"
                   "  Spawn(\n"
                   "      a_long_argument,\n"
                   "      another_long_argument,\n"
                   "      a_third_long_argument,\n"
                   "      and_a_fourth_one)\n"
                   "}\n");
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, BreaksLongParameterLists) {
  Pass pass(
      "fn LongFunctionName(first_parameter: SomeLongType, "
      "second_parameter: AnotherType, third: int) {}\n");
  WOLANG_EXPECT(pass.formatted().find("(\n") != std::string::npos);
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, KeepsComments) {
  Pass pass(
      "// leading\n"
      "fn f() {\n"
      "  // inside\n"
      "  let a = 1  // trailing\n"
      "  // before the end\n"
      "}\n"
      "// at the end of the file\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "// leading\n"
                   "fn f() {\n"
                   "  // inside\n"
                   "  let a = 1  // trailing\n"
                   "  // before the end\n"
                   "}\n"
                   "// at the end of the file\n");
}

WOLANG_TEST(Formatter, KeepsDocumentationComments) {
  // The text after `///` is prose, so its own indentation is left alone; only
  // the indentation of the comment line itself is normalized.
  Pass pass(
      "  /// Fires once.\n"
      "  ///   Indented continuation.\n"
      "fn Fire() {}\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "/// Fires once.\n"
                   "///   Indented continuation.\n"
                   "fn Fire() {}\n");
}

WOLANG_TEST(Formatter, PutsAttributesAboveTheDeclaration) {
  Pass pass("@Edit @Range(1, 50) var range: float = 1.0\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "@Edit @Range(1, 50)\n"
                   "var range: float = 1.0\n");
}

WOLANG_TEST(Formatter, FormatsControlFlow) {
  Pass pass(
      "fn f() {\n"
      "if a {b()} else if c {d()} else {e()}\n"
      "while x {y()}\n"
      "for i in 0..10 {g(i)}\n"
      "loop {h()}\n"
      "}\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "fn f() {\n"
                   "  if a {\n"
                   "    b()\n"
                   "  } else if c {\n"
                   "    d()\n"
                   "  } else {\n"
                   "    e()\n"
                   "  }\n"
                   "  while x {\n"
                   "    y()\n"
                   "  }\n"
                   "  for i in 0..10 {\n"
                   "    g(i)\n"
                   "  }\n"
                   "  loop {\n"
                   "    h()\n"
                   "  }\n"
                   "}\n");
}

WOLANG_TEST(Formatter, FormatsMatchArms) {
  Pass pass(
      "fn f() {\n"
      "  match s {\n"
      "    Idle=>Look()\n"
      "    Chase(t)|Attack(t,_) if ready=>Move(t)\n"
      "    _=>{Stop()}\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "fn f() {\n"
                   "  match s {\n"
                   "    Idle => Look()\n"
                   "    Chase(t) | Attack(t, _) if ready => Move(t)\n"
                   "    _ => {\n"
                   "      Stop()\n"
                   "    }\n"
                   "  }\n"
                   "}\n");
}

WOLANG_TEST(Formatter, FormatsStructPatternsInTheirShortForm) {
  // `x: x` means the same as `x`, so the short form is canonical; `y: inner`
  // binds a different name and has to survive.
  Pass pass("fn f() {\n  match p {\n    Point { x: x, y: inner } => 1\n  }\n}\n");
  WOLANG_EXPECT(pass.formatted().find("Point { x, y: inner }") !=
                std::string::npos);
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, FormatsStateMachines) {
  Pass pass(
      "machine Door{state Closed{on Interact->Opening}\n"
      "state Open{enter{Play()}on Interact after 3s->Closed}}\n");
  WOLANG_EXPECT_EQ(pass.formatted(),
                   "machine Door {\n"
                   "  state Closed {\n"
                   "    on Interact -> Opening\n"
                   "  }\n"
                   "  state Open {\n"
                   "    enter {\n"
                   "      Play()\n"
                   "    }\n"
                   "    on Interact after 3s -> Closed\n"
                   "  }\n"
                   "}\n");
}

WOLANG_TEST(Formatter, FormatsStringsAndInterpolations) {
  Pass pass("let s = \"hp {hp*2} / {max}\\n\"\n");
  WOLANG_EXPECT_EQ(pass.formatted(), "let s = \"hp {hp * 2} / {max}\\n\"\n");
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, FormatsTypes) {
  Pass pass(
      "fn f(a:Actor?,b:Array<float>,c:fn(int)->bool,d:*mut float,"
      "e:&Transform,g:(int,bool),h:Query<mut T,with Tag>) {}\n");
  WOLANG_EXPECT(pass.formatted().find("a: Actor?") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("c: fn(int) -> bool") !=
                std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("d: *mut float") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("e: &Transform") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("h: Query<mut T, with Tag>") !=
                std::string::npos);
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, FormatsTheNewSurface) {
  WOLANG_EXPECT_STABLE(
      "error NotFound, Corrupt\n"
      "extend Vec3 {\n  fn Flat() -> Vec3 => self\n}\n"
      "extend Barrel as Damageable {\n"
      "  fn TakeDamage(amount: float) {}\n"
      "}\n"
      "fn Find(id: int) -> Pawn?\n"
      "fn Load(p: str) -> Level!\n"
      "fn Lookup(k: str) -> Pawn?!\n"
      "fn Sum(xs: [float]) -> float\n"
      "fn Fixed(xs: [float; 3])\n"
      "var burst: soa [Particle]\n"
      "var packed: aos [Particle]\n");
}

WOLANG_TEST(Formatter, FormatsLabelledLoopsAndRanges) {
  Pass pass(
      "fn f() {\n"
      "outer: loop {\n"
      "for i in 2.. { break outer }\n"
      "let a = xs[..5]\n"
      "let b = xs[..]\n"
      "continue outer\n"
      "}\n"
      "}\n");
  WOLANG_EXPECT(pass.formatted().find("outer: loop {") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("for i in 2.. {") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("xs[..5]") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("xs[..]") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("break outer") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("continue outer") != std::string::npos);
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, FormatsOptionalPatterns) {
  Pass pass(
      "fn f() {\n"
      "  match count {\n"
      "    n? => n\n"
      "    null => 0\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(pass.formatted().find("n? => n") != std::string::npos);
  WOLANG_EXPECT(pass.formatted().find("null => 0") != std::string::npos);
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, FormatsStructPatternsInConditions) {
  Pass pass("fn f() { if let Point { x, y: inner } = p { Use(x) } }");
  WOLANG_EXPECT(pass.formatted().find("if let Point { x, y: inner } = p") !=
                std::string::npos);
  WOLANG_EXPECT_STABLE(pass.formatted());
}

WOLANG_TEST(Formatter, KeepsBlockComments) {
  Pass pass("/* a note */\nfn f() {}\n");
  WOLANG_EXPECT_EQ(pass.formatted(), "/* a note */\nfn f() {}\n");
}

WOLANG_TEST(Formatter, FormatsEveryDeclarationKind) {
  WOLANG_EXPECT_STABLE(
      "import a.b\n"
      "import a.c.{D, E}\n"
      "dimension Length, Time\n"
      "unit m: Length\n"
      "unit cm = 0.01m\n"
      "effect Telemetry\n"
      "type Health = int\n"
      "tag Falling\n"
      "component Velocity {\n  var value: Vec3\n}\n"
      "enum State {\n  Idle\n  Chase(target: Pawn)\n}\n"
      "trait Damageable {\n  fn TakeDamage(amount: float)\n}\n"
      "extend Barrel as Damageable {\n"
      "  fn TakeDamage(amount: float) {}\n"
      "}\n"
      "struct Hit {\n  var point: Vec3 = origin\n}\n"
      "signal Died(by: Actor?)\n"
      "system Integrate(dt: float) after Gravity {\n  Step(dt)\n}\n"
      "pub async fn Run<T: Actor>(x: T) -> T? uses io {\n  return null\n}\n"
      "extern \"C\" fn Noise(x: float) -> float = \"wolang_noise\"\n"
      "comptime let Table = Build()\n");
}

WOLANG_TEST(Formatter, StaysStableOnTheBundledExamples) {
  for (std::string_view name :
       {"examples/turret.wo", "examples/hello.wo"}) {
    SourceManager sources;
    std::string path = WOLANG_PROJECT_DIR;
    path.push_back('/');
    path.append(name);

    std::string error;
    const FileId file = sources.LoadFile(path, &error);
    WOLANG_ASSERT(file.is_valid());
    ExpectStable(sources.Text(file), __FILE__, __LINE__);
  }
}

WOLANG_TEST(Formatter, HonoursTheColumnLimit) {
  FormatOptions narrow;
  narrow.column_limit = 20;
  Pass pass("fn f() {\n  Call(first, second)\n}\n", narrow);
  WOLANG_EXPECT(pass.formatted().find("Call(\n") != std::string::npos);

  FormatOptions wide;
  wide.column_limit = 80;
  Pass unbroken("fn f() {\n  Call(first, second)\n}\n", wide);
  WOLANG_EXPECT(unbroken.formatted().find("Call(first, second)") !=
                std::string::npos);
}

}  // namespace
}  // namespace wolang
