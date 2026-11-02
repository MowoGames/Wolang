// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/sema/type_check.h"

#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/compiler/database.h"

namespace wolang {
namespace {

// Type checks one snippet through the database, so the test exercises the
// same path the driver does.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  Database& db() { return db_; }
  FileId file() const { return file_; }

  // Errors only. A warning is asserted through Warnings(), so that adding a
  // lint does not invalidate every test that says "this compiles".
  std::string Codes() { return CodesOf(Severity::kError); }
  std::string Warnings() { return CodesOf(Severity::kWarning); }

  std::string CodesOf(Severity severity) {
    std::string result;
    const std::vector<Diagnostic>* diagnostics = db_.Diagnostics(file_);
    if (diagnostics == nullptr) {
      return result;
    }
    for (const Diagnostic& diagnostic : *diagnostics) {
      if (diagnostic.severity != severity) {
        continue;
      }
      if (!result.empty()) {
        result.push_back(',');
      }
      result.append(diagnostic.code);
    }
    return result;
  }

  std::string Messages() {
    std::string result;
    const std::vector<Diagnostic>* diagnostics = db_.Diagnostics(file_);
    if (diagnostics == nullptr) {
      return result;
    }
    for (const Diagnostic& diagnostic : *diagnostics) {
      result.append(diagnostic.message);
      result.push_back('\n');
    }
    return result;
  }

  bool HasCode(std::string_view code) {
    return Codes().find(code) != std::string::npos;
  }

  // The type of the initializer of the named declaration, rendered the way a
  // diagnostic would show it.
  std::string TypeOfInit(std::string_view name) {
    const ParsedFile* parsed = db_.Parse(file_);
    const CheckedFile* checked = db_.Checked(file_);
    const FileResolution* resolution = db_.Resolution(file_);
    if (parsed == nullptr || checked == nullptr || resolution == nullptr) {
      return "<none>";
    }
    const Symbol symbol = db_.interner().Lookup(name);
    const Ast& ast = parsed->ast;

    // Walk every declaration and statement looking for the binding.
    for (ItemId id : ast.Get(ast.root())) {
      const std::string found = FindInItem(ast, *checked, *resolution, id,
                                           symbol);
      if (!found.empty()) {
        return found;
      }
    }
    return "<not found>";
  }

 private:
  std::string Render(const CheckedFile& checked,
                     const FileResolution& resolution,
                     ExprId expr) {
    return db_.types().Describe(db_.interner(), checked.body.TypeOf(expr));
  }

  std::string FindInItem(const Ast& ast,
                         const CheckedFile& checked,
                         const FileResolution& resolution,
                         ItemId id,
                         Symbol name) {
    if (!ast.Contains(id)) {
      return std::string();
    }
    const Item& item = ast.Get(id);
    if (item.kind == ItemKind::kBinding && item.name == name &&
        item.init.is_valid()) {
      return Render(checked, resolution, item.init);
    }
    if (item.body.is_valid()) {
      const std::string found =
          FindInBlock(ast, checked, resolution, item.body, name);
      if (!found.empty()) {
        return found;
      }
    }
    for (ItemId member : ast.Get(item.members)) {
      const std::string found =
          FindInItem(ast, checked, resolution, member, name);
      if (!found.empty()) {
        return found;
      }
    }
    return std::string();
  }

  // Searches a block and everything nested inside it, so a binding in a loop
  // or a branch is found too.
  std::string FindInBlock(const Ast& ast,
                          const CheckedFile& checked,
                          const FileResolution& resolution,
                          ExprId expr_id,
                          Symbol name) {
    if (!ast.Contains(expr_id)) {
      return std::string();
    }
    const Expr& expr = ast.Get(expr_id);

    if (expr.kind == ExprKind::kBlock) {
      for (StmtId stmt_id : ast.Get(expr.stmts)) {
        const Stmt& stmt = ast.Get(stmt_id);
        if ((stmt.kind == StmtKind::kLet || stmt.kind == StmtKind::kVar) &&
            stmt.pattern.is_valid() && stmt.expr.is_valid()) {
          const Pattern& pattern = ast.Get(stmt.pattern);
          if (pattern.kind == PatternKind::kBinding &&
              pattern.name == name) {
            return Render(checked, resolution, stmt.expr);
          }
        }
        if (stmt.expr.is_valid()) {
          const std::string found =
              FindInBlock(ast, checked, resolution, stmt.expr, name);
          if (!found.empty()) {
            return found;
          }
        }
      }
      return std::string();
    }

    for (ExprId child : {expr.lhs, expr.rhs, expr.extra}) {
      if (!child.is_valid()) {
        continue;
      }
      const std::string found =
          FindInBlock(ast, checked, resolution, child, name);
      if (!found.empty()) {
        return found;
      }
    }
    for (MatchArmId arm_id : ast.Get(expr.arms)) {
      const std::string found = FindInBlock(
          ast, checked, resolution, ast.Get(arm_id).body, name);
      if (!found.empty()) {
        return found;
      }
    }
    return std::string();
  }

  Database db_;
  FileId file_;
};

// ----------------------------------------------------------- inference -----

WOLANG_TEST(TypeCheck, IntegerLiteralsTakeTheirContextsType) {
  Fixture fixture(
      "fun f() {\n"
      "  const a = 1\n"
      "  const b: u8 = 200\n"
      "  const c: s64 = 1\n"
      "  const d: f64 = 2\n"
      "  const e: s64 = 0xFFFFFFFFFFFF\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  // Without an expectation a literal settles on `int`.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "s64");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "u8");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "s64");
  // An integer literal in a float context is fine.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("d"), "f64");
  // Arbitrary precision until used, so no suffix is needed.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("e"), "s64");
}

WOLANG_TEST(TypeCheck, ReportsALiteralThatDoesNotFit) {
  WOLANG_EXPECT(Fixture("fun f() { const x: u8 = 300 }").HasCode("E0109"));
  WOLANG_EXPECT(Fixture("fun f() { const x: s8 = 200 }").HasCode("E0109"));
  // The boundary values fit.
  WOLANG_EXPECT(Fixture("fun f() { const x: u8 = 255 }").Codes().empty());
  WOLANG_EXPECT(Fixture("fun f() { const x: s8 = 127 }").Codes().empty());
}

WOLANG_TEST(TypeCheck, TheMinimumOfASignedTypeNeedsItsMinusSign) {
  WOLANG_EXPECT(Fixture("fun f() { const x: s8 = 128 }").HasCode("E0109"));
  WOLANG_EXPECT(Fixture("fun f() { const x: s64 = 9223372036854775808 }").HasCode("E0109"));
  WOLANG_EXPECT(Fixture("fun f() { const x: s8 = -128 }").Codes().empty());
  WOLANG_EXPECT(Fixture("fun f() { const x: s16 = -32768 }").Codes().empty());
  WOLANG_EXPECT(Fixture("fun f() { const x: s8 = -129 }").HasCode("E0109"));
}

WOLANG_TEST(TypeCheck, AWideLiteralWithNoTypeIsNotCutToInt) {
  // `int` is s64 (D238), so the edge is 2^63; it used to wrap 4294967297 to
  // 1 without a word (D225).
  WOLANG_EXPECT(Fixture("fun f() { const x = 9223372036854775808 }")
                    .HasCode("E0109"));
  WOLANG_EXPECT(Fixture("fun f() { const x = -9223372036854775809 }")
                    .HasCode("E0109"));
  WOLANG_EXPECT(!Fixture("fun f() { const x = 4294967297 }").HasCode("E0109"));
  WOLANG_EXPECT(!Fixture("fun f() { const x = 2147483648 }").HasCode("E0109"));
  WOLANG_EXPECT(!Fixture("fun f() { const x = -9223372036854775808 }")
                     .HasCode("E0109"));
  WOLANG_EXPECT(!Fixture("fun f() { const x: u64 = 9223372036854775808 }")
                     .HasCode("E0109"));
  WOLANG_EXPECT(!Fixture("fun f() { const x = 4294967297 as s64 }")
                     .HasCode("E0109"));
  WOLANG_EXPECT(!Fixture("fun f() { const x = 4294967297 as u64 }")
                     .HasCode("E0109"));
}

WOLANG_TEST(TypeCheck, LenIsInt) {
  // A length is `int`, which is s64 (D238).
  WOLANG_EXPECT(Fixture("fun f(xs: []int) { const n: s64 = xs.len }")
                    .Codes()
                    .empty());
  WOLANG_EXPECT(Fixture("fun f(xs: []int) { const n: s32 = xs.len }")
                    .HasCode("E0101"));
  WOLANG_EXPECT(Fixture("fun f(t: text) { const n: s64 = t.len }")
                    .Codes()
                    .empty());
  WOLANG_EXPECT(Fixture("fun f(t: text) { const n: s32 = t.len }")
                    .HasCode("E0101"));
  WOLANG_EXPECT(Fixture("fun f(a: [3]int) { const n: s64 = a.len }")
                    .Codes()
                    .empty());
  WOLANG_EXPECT(Fixture("fun f(a: [3]int) { const n: s32 = a.len }")
                    .HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, AnIntDoesNotReachAnS32ExternParameter) {
  // C's `int` is `s32`, so the width is spelled at the boundary (D238).
  const std::string declaration = "extern \"C\" fun abs(n: s32) -> s32\n";
  WOLANG_EXPECT(Fixture(declaration + "fun f(x: int) { const y = abs(x) }")
                    .HasCode("E0101"));
  WOLANG_EXPECT(
      Fixture(declaration + "fun f(x: int) { const y = abs(x as s32) }")
          .Codes()
          .empty());
}

WOLANG_TEST(TypeCheck, ALiteralBelowTheS32RangeDoesNotFitAnS32) {
  // `int` grew, `s32` did not: the old boundary still holds for it (D238).
  WOLANG_EXPECT(Fixture("fun f() { const x: s32 = -2147483649 }")
                    .HasCode("E0109"));
  WOLANG_EXPECT(Fixture("fun f() { const x: s32 = -2147483648 }")
                    .Codes()
                    .empty());
}

WOLANG_TEST(TypeCheck, LiteralArithmeticTakesTheContextsType) {
  WOLANG_EXPECT_EQ(
      Fixture("fun f() { const m: s64 = -9223372036854775807 - 1\n"
              "  const _n = m }").Codes(),
      "");
}

WOLANG_TEST(TypeCheck, SuffixesFixTheTypeRegardlessOfContext) {
  Fixture fixture(
      "fun f() {\n"
      "  const a = 1u8\n"
      "  const b = 2.0f64\n"
      "  const c = 3s64\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "u8");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "f64");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "s64");
}

WOLANG_TEST(TypeCheck, InfersLiteralKinds) {
  Fixture fixture(
      "fun f() {\n"
      "  const a = true\n"
      "  const b = \"text\"\n"
      "  const c = 'x'\n"
      "  const d = 1.5\n"
      "  const e = \"a {a}\"\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "bool");
  // Static data, so a borrowed `str` rather than an allocation.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "text");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "char");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("d"), "f32");
  // Interpolation builds a new string at runtime, which is what makes it
  // owned.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("e"), "Text");
}

WOLANG_TEST(TypeCheck, ReportsMismatchedTypes) {
  WOLANG_EXPECT(
      Fixture("fun f() { const x: bool = 1 }").HasCode("E0101"));
  WOLANG_EXPECT(
      Fixture("fun f() { const x: int = true }").HasCode("E0101"));
  WOLANG_EXPECT(
      Fixture("fun f() -> int { return true }").HasCode("E0101"));
  // A float literal does not silently become an integer.
  WOLANG_EXPECT(Fixture("fun f() { const x: int = 1.5 }").HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, ChecksOperators) {
  Fixture fixture(
      "fun f(flag: bool, n: int) {\n"
      "  const a = n + 1\n"
      "  const b = n < 2\n"
      "  const c = flag && true\n"
      "  const d = -n\n"
      "  const e = !flag\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "s64");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "bool");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "bool");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("d"), "s64");

  WOLANG_EXPECT(Fixture("fun f(s: text) { const a = s + 1 }").HasCode("E0108"));
  WOLANG_EXPECT(Fixture("fun f(s: text) { const a = -s }").HasCode("E0108"));
  WOLANG_EXPECT(Fixture("fun f(n: int) { const a = n && true }")
                    .HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, LiteralsOnEitherSideTakeTheOthersType) {
  Fixture fixture(
      "fun f(small: u8) {\n"
      "  const a = small + 1\n"
      "  const b = 1 + small\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "u8");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "u8");
}

// -------------------------------------------------------------- scopes -----

WOLANG_TEST(TypeCheck, ReportsAnUnknownName) {
  Fixture fixture("fun f() { const x = nothing_here }");
  WOLANG_EXPECT(fixture.HasCode("E0103"));
  WOLANG_EXPECT(fixture.Messages().find("no value named `nothing_here`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, ParametersAndLocalsAreInScope) {
  Fixture fixture(
      "fun f(a: int) {\n"
      "  const b = a\n"
      "  const c = b\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "s64");
}

WOLANG_TEST(TypeCheck, ALocalDoesNotEscapeItsBlock) {
  Fixture fixture(
      "fun f() {\n"
      "  if true {\n"
      "    const inner = 1\n"
      "  }\n"
      "  const x = inner\n"
      "}\n");
  WOLANG_EXPECT(fixture.HasCode("E0103"));
}

WOLANG_TEST(TypeCheck, ReportsAssignmentToALetBinding) {
  WOLANG_EXPECT(
      Fixture("fun f() {\n  const a = 1\n  a = 2\n}\n").HasCode("E0105"));
  // A `var` may be reassigned.
  WOLANG_EXPECT(
      Fixture("fun f() {\n  var a = 1\n  a = 2\n}\n").Codes().empty());
}

WOLANG_TEST(TypeCheck, ChecksAssignedTypes) {
  WOLANG_EXPECT(
      Fixture("fun f() {\n  var a = 1\n  a = true\n}\n").HasCode("E0101"));
}

// --------------------------------------------------------- nullability -----

WOLANG_TEST(TypeCheck, OptionalsAcceptTheirElement) {
  Fixture fixture(
      "struct Pawn {}\n"
      "fun f(p: Pawn) {\n"
      "  const a: Pawn? = p\n"
      "  const b: Pawn? = null\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "Pawn?");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "Pawn?");
}

WOLANG_TEST(TypeCheck, ReportsAFieldAccessThroughAnOptional) {
  Fixture fixture(
      "struct Pawn {}\n"
      "fun f(p: Pawn?) {\n"
      "  const a = p.field\n"
      "}\n");
  WOLANG_EXPECT(fixture.HasCode("E0104"));
  WOLANG_EXPECT(fixture.Messages().find("may be absent") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, ACallThroughAnOptionalGivesAnOptional) {
  Fixture fixture(
      "class Pawn {\n"
      "  var hp: int\n"
      "  fun Health() -> int => hp\n"
      "  fun Hit() {}\n"
      "}\n"
      "fun f(p: Pawn?) {\n"
      "  const a = p?.Health()\n"
      "  p?.Hit()\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "s64?");

  WOLANG_EXPECT(Fixture(
      "class Pawn { fun Health() -> int => 1 }\n"
      "fun f(p: Pawn?) { const a = p.Health() }\n").HasCode("E0104"));
  WOLANG_EXPECT(!Fixture(
      "class Pawn { fun Health() -> int => 1 }\n"
      "fun f(p: Pawn?) { const a = p?.Health() }\n").HasCode("E0106"));
}

WOLANG_TEST(TypeCheck, IfLetNarrowsAnOptional) {
  Fixture fixture(
      "struct Pawn {}\n"
      "fun f(p: Pawn?) {\n"
      "  if const inner = p {\n"
      "    const a = inner\n"
      "  }\n"
      "}\n");
  // The narrowed binding is no longer optional, so using it is fine.
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "Pawn");
}

WOLANG_TEST(TypeCheck, AClosureTakesItsParameterTypesFromTheContext) {
  Fixture fixture(
      "fun Apply(f: fun(int) -> int, x: int) -> int => f(x)\n"
      "fun Use() -> int => Apply(|n| n + 1, 2)\n");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(TypeCheck, AClosureWithNoContextNeedsItsTypesWritten) {
  Fixture fixture("fun Use() { const f = |n| n + 1 }\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0134");
}

WOLANG_TEST(TypeCheck, AClosureTakesACopyOfWhatItReads) {
  // D138: a closure captures by copy, and its environment lives in the
  // frame of the call that made it -- so passing it *down* is fine.
  Fixture fixture(
      "fun Apply(f: fun(int) -> int, x: int) -> int => f(x)\n"
      "fun Use() -> int {\n  const k = 10\n"
      "  return Apply(|n| n + k, 1)\n}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(TypeCheck, ACapturingClosureMayOutliveItsFrame) {
  // D240: where the result is kept, the closure owns what it captured.
  Fixture returned(
      "fun Make() -> fun(int) -> int {\n  const k = 10\n"
      "  return |n| n + k\n}\n");
  WOLANG_EXPECT(returned.Codes().empty());

  // And a closure that captures nothing is a plain function: it may go
  // anywhere.
  Fixture plain(
      "fun Make() -> fun(int) -> int {\n  return |n| n + 1\n}\n");
  WOLANG_EXPECT(plain.Codes().empty());
}

WOLANG_TEST(TypeCheck, ABorrowedFunctionCannotBeStored) {
  Fixture fixture(
      "fun Inc(n: int) -> int => n + 1\n"
      "var saved: fun(int) -> int = Inc\n"
      "fun Hold(f: fun(int) -> int) {\n  saved = f\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0161");

  Fixture kept(
      "fun Inc(n: int) -> int => n + 1\n"
      "var saved: fun(int) -> int = Inc\n"
      "fun Hold(f: keep fun(int) -> int) {\n  saved = f\n}\n");
  WOLANG_EXPECT(kept.Codes().empty());
}

WOLANG_TEST(TypeCheck, ABorrowedFunctionCannotBeReturned) {
  Fixture fixture("fun Pass(f: fun() -> int) -> fun() -> int {\n  return f\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0161");

  Fixture kept("fun Pass(f: keep fun() -> int) -> fun() -> int {\n  return f\n}\n");
  WOLANG_EXPECT(kept.Codes().empty());
}

WOLANG_TEST(TypeCheck, ABorrowedFunctionCannotGoToAKeptParameter) {
  Fixture fixture(
      "fun Store(f: keep fun()) {}\n"
      "fun Relay(f: fun()) {\n  Store(f)\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0161");

  Fixture down(
      "fun Run(f: fun()) {}\n"
      "fun Relay(f: fun()) {\n  Run(f)\n}\n");
  WOLANG_EXPECT(down.Codes().empty());

  Fixture plain(
      "fun Store(f: keep fun()) {}\n"
      "fun Nothing() {}\n"
      "fun Use() {\n  Store(Nothing)\n}\n");
  WOLANG_EXPECT(plain.Codes().empty());
}

WOLANG_TEST(TypeCheck, ABorrowedFunctionCannotFillAField) {
  Fixture fixture(
      "class Box {\n  var f: fun()\n}\n"
      "fun Fill(f: fun()) {\n  const b = Box(f)\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0161");
}

WOLANG_TEST(TypeCheck, AnEscapingClosureCannotTakeAReference) {
  Fixture fixture(
      "fun Make(r: &int) -> fun() {\n  return || { const held = r }\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0162");

  Fixture value(
      "fun Make(r: int) -> fun() -> int {\n  return || r\n}\n");
  WOLANG_EXPECT(value.Codes().empty());
}

WOLANG_TEST(TypeCheck, AnEscapingClosureMovesAValueWithADeinit) {
  Fixture fires(R"(
struct Res {
  var id: int
  deinit {}
}

fun Make() -> fun() -> int {
  const r = Res(1)
  const g: fun() -> int = || r.id
  const again = r.id
  return g
}
)");
  WOLANG_EXPECT(fires.HasCode("E0122"));

  Fixture fine(R"(
struct Res {
  var id: int
  deinit {}
}

fun Make() -> fun() -> int {
  const r = Res(1)
  const g: fun() -> int = || r.id
  return g
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0122"));

  Fixture borrowed(R"(
struct Res {
  var id: int
  deinit {}
}

fun Use() -> int {
  const r = Res(1)
  const g = || r.id
  return r.id + g()
}
)");
  WOLANG_EXPECT(!borrowed.HasCode("E0122"));
}

WOLANG_TEST(TypeCheck, ACapturingClosureHeldLocallyIsBorrowed) {
  Fixture fixture(
      "fun Make() -> fun() -> int {\n  const k = 1\n"
      "  const g = || k\n  return g\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0161");
}

WOLANG_TEST(TypeCheck, ACaptureOutsideAClosureIsStillAnError) {
  // A name from nowhere is a name from nowhere.
  Fixture fixture("fun Use() -> int {\n  return missing + 1\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0103");
}

WOLANG_TEST(TypeCheck, PresentPatternNarrowsAnOptional) {
  // The `return` is the point of D99: a block gives nothing back by
  // itself, so a function that declares a result says `return`.
  Fixture fixture(
      "fun f(count: int?) -> int {\n"
      "  return match count {\n"
      "    n? => n\n"
      "    null => 0\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(TypeCheck, NullCoalescingRemovesOneLayer) {
  Fixture fixture(
      "fun f(count: int?) {\n"
      "  const a = count ?? 0\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "s64");

  WOLANG_EXPECT(
      Fixture("fun f(n: int) { const a = n ?? 0 }").HasCode("E0108"));
  WOLANG_EXPECT(
      Fixture("fun f(n: int?) { const a = n ?? true }").HasCode("E0101"));
}

// ---------------------------------------------------------------- units -----

WOLANG_TEST(TypeCheck, CompoundAssignmentIsHeldToTheOperatorsOperands) {
  // `a op= b` is `a = a op b`, so it asks the same of its operands (D246).
  Fixture ok(
      "fun f(o: int?) {\n"
      "  var n = 1\n"
      "  n += 2\n"
      "  n <<= 1\n"
      "  var m = o\n"
      "  m ?\?= 3\n"
      "  var x = 1.5\n"
      "  x *= 2.0\n"
      "}\n");
  WOLANG_EXPECT(ok.Codes().empty());

  WOLANG_EXPECT(Fixture("fun f() { var b = true\n b += 1 }").HasCode("E0108"));
  WOLANG_EXPECT(Fixture("fun f() { var x = 1.5\n x &= 1 }").HasCode("E0108"));
  WOLANG_EXPECT(Fixture("fun f() { var n = 1\n n ?\?= 2 }").HasCode("E0108"));
  WOLANG_EXPECT(Fixture("fun f(o: int?) { var m = o\n m ?\?= true }")
                    .HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, UnitLiteralsCarryTheirDimension) {
  Fixture fixture(
      "dimension Length\n"
      "dimension Time\n"
      "unit m: Length\n"
      "unit s: Time\n"
      "fun f() {\n"
      "  const distance = 10m\n"
      "  const duration = 2s\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("distance"), "Length");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("duration"), "Time");
}

WOLANG_TEST(TypeCheck, DividingQuantitiesCombinesDimensions) {
  Fixture fixture(
      "dimension Length\n"
      "dimension Time\n"
      "unit m: Length\n"
      "unit s: Time\n"
      "fun f() {\n"
      "  const speed = 10m / 2s\n"
      "  const area = 3m * 4m\n"
      "  const ratio = 10m / 2m\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  // The type nobody declared.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("speed"), "Length/Time");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("area"), "Length^2");
  // Everything cancels, so the result is a plain number.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("ratio"), "f32");
}

WOLANG_TEST(TypeCheck, AddingDifferentDimensionsIsAnError) {
  Fixture fixture(
      "dimension Length\n"
      "dimension Time\n"
      "unit m: Length\n"
      "unit s: Time\n"
      "fun f() {\n"
      "  const bad = 10m + 2s\n"
      "}\n");
  WOLANG_EXPECT(fixture.HasCode("E1001"));
  WOLANG_EXPECT(fixture.Messages().find("cannot combine `Length` with "
                                        "`Time`") != std::string::npos);
}

WOLANG_TEST(TypeCheck, ComparingDifferentDimensionsIsAnError) {
  Fixture fixture(
      "dimension Length\n"
      "dimension Time\n"
      "unit m: Length\n"
      "unit s: Time\n"
      "fun f() { const bad = 10m < 2s }\n");
  WOLANG_EXPECT(fixture.HasCode("E1001"));
}

WOLANG_TEST(TypeCheck, SameDimensionAddsFine) {
  Fixture fixture(
      "dimension Length\n"
      "unit m: Length\n"
      "unit cm = 0.01m\n"
      "fun f() {\n"
      "  const total = 10m + 50cm\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  // A derived unit shares its base unit's dimension.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("total"), "Length");
}

WOLANG_TEST(TypeCheck, ReportsAnUnknownUnit) {
  Fixture fixture("fun f() { const x = 10parsecs }");
  WOLANG_EXPECT(fixture.HasCode("E1002"));
  WOLANG_EXPECT(fixture.Messages().find("no unit named `parsecs`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, ReportsAUnitWithAnUnknownDimension) {
  Fixture fixture("unit m: Nothing\n");
  WOLANG_EXPECT(fixture.HasCode("E0114"));
}

WOLANG_TEST(TypeCheck, ReportsABadUnitConversion) {
  // A derived unit has to convert through a literal carrying a unit.
  WOLANG_EXPECT(Fixture("unit cm = 5\n").HasCode("E0115"));
}

// ---------------------------------------------------------------- types -----

WOLANG_TEST(TypeCheck, ResolvesDeclaredTypes) {
  Fixture fixture(
      "struct Vec3 {}\n"
      "class Turret {}\n"
      "fun f(a: Vec3, b: Turret?, c: []Vec3, d: [3]Vec3) {\n"
      "  const x = a\n"
      "  const y = c\n"
      "  const z = d\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("x"), "Vec3");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("y"), "[]Vec3");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("z"), "[3]Vec3");
}

WOLANG_TEST(TypeCheck, ReportsAnUnknownType) {
  Fixture fixture("fun f(a: Nothing) {}");
  WOLANG_EXPECT(fixture.HasCode("E0110"));
  WOLANG_EXPECT(fixture.Messages().find("no type named `Nothing`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, ReportsAValueUsedAsAType) {
  Fixture fixture("fun value() {}\nfun f(a: value) {}\n");
  WOLANG_EXPECT(fixture.HasCode("E0111"));
}

WOLANG_TEST(TypeCheck, GenericParametersShadowTypes) {
  Fixture fixture(
      "struct T {}\n"
      "fun f<T>(a: T) {\n"
      "  const x = a\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  // The parameter shadows the struct, as the innermost scope.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("x"), "T");
}

WOLANG_TEST(TypeCheck, ArrayLiteralsTakeTheirElementType) {
  Fixture fixture(
      "fun f() {\n"
      "  const a = [1, 2, 3]\n"
      "  const b: [2]u8 = [1, 2]\n"
      "  const c = [1.5, 2.5]\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "[3]s64");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "[2]u8");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "[2]f32");

  WOLANG_EXPECT(
      Fixture("fun f() { const a = [1, true] }").HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, IndexingYieldsTheElementType) {
  Fixture fixture(
      "fun f(xs: []float) {\n"
      "  const a = xs[0]\n"
      "  const b = xs[1..]\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "f32");
  // Indexing with a range produces a slice.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "[]f32");

  WOLANG_EXPECT(Fixture("fun f(n: int) { const a = n[0] }").HasCode("E0107"));
}

WOLANG_TEST(TypeCheck, ForBindsTheElementType) {
  Fixture fixture(
      "fun f(xs: []float) {\n"
      "  for item in xs {\n"
      "    const a = item\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "f32");
}

WOLANG_TEST(TypeCheck, TuplesCheckElementwise) {
  Fixture fixture(
      "fun f() {\n"
      "  const a = (1, true)\n"
      "  const b: (u8, bool) = (2, false)\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "(s64, bool)");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "(u8, bool)");
}

WOLANG_TEST(TypeCheck, ExpressionBodiedFunctionsCheckAgainstTheirReturn) {
  WOLANG_EXPECT(
      Fixture("fun f() -> int => 1").Codes().empty());
  WOLANG_EXPECT(
      Fixture("fun f() -> int => true").HasCode("E0101"));
  WOLANG_EXPECT(
      Fixture("fun Lerp(a: float, b: float, t: float) -> float "
              "=> a + (b - a) * t")
          .Codes()
          .empty());
}

WOLANG_TEST(TypeCheck, IfElseChainsAgree) {
  Fixture fixture(
      "fun f(flag: bool) {\n"
      "  const a = if flag { 1 } else { 2 }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "s64");

  WOLANG_EXPECT(
      Fixture("fun f(flag: bool) { const a = if flag { 1 } else { true } }")
          .HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, CastsTakeTheTargetType) {
  Fixture fixture(
      "fun f(n: int) {\n"
      "  const a = n as s64\n"
      "  const b = n as f64\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "s64");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "f64");
}

WOLANG_TEST(TypeCheck, ErrorTypesDoNotCascade) {
  // One unknown name produces exactly one message, not one per use.
  Fixture fixture(
      "fun f() {\n"
      "  const a = unknown\n"
      "  const b = a + 1\n"
      "  const c = b * 2\n"
      "  const d: bool = c\n"
      "}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0103");
}

// ------------------------------------------------------- incrementality -----

WOLANG_TEST(TypeCheck, EditingABodyLeavesResolutionUnchanged) {
  Fixture fixture("fun f() -> int {\n  return 1\n}\n");
  fixture.db().Checked(fixture.file());
  const QueryKey key{QueryKind::kResolution, fixture.file().index};
  const Revision before = fixture.db().engine().ChangedAt(key);
  fixture.db().engine().ResetCounters();

  fixture.db().SetFileText(fixture.file(),
                           "fun f() -> int {\n  return 2\n}\n");
  fixture.db().Checked(fixture.file());

  // Resolution reads the syntax tree, so a body edit does recompute it, but
  // the value comes out equal -- which is what keeps everything downstream of
  // it green.
  WOLANG_EXPECT_EQ(
      fixture.db().engine().ComputationCount(QueryKind::kTypeCheck),
      uint64_t{1});
  WOLANG_EXPECT_EQ(fixture.db().engine().ChangedAt(key), before);
}

WOLANG_TEST(TypeCheck, EditingASignatureChangesResolution) {
  Fixture fixture("fun f() -> int {\n  return 1\n}\n");
  fixture.db().Checked(fixture.file());
  const QueryKey key{QueryKind::kResolution, fixture.file().index};
  const Revision before = fixture.db().engine().ChangedAt(key);

  fixture.db().SetFileText(fixture.file(),
                           "fun g() -> bool {\n  return true\n}\n");
  fixture.db().Checked(fixture.file());

  WOLANG_EXPECT_GT(fixture.db().engine().ChangedAt(key), before);
}


// ------------------------------------------------------------- members -----

WOLANG_TEST(TypeCheck, FieldAccessTakesTheFieldType) {
  Fixture f(R"(
struct Vec3 {
  var x: float
  var y: float
}

fun take(v: Vec3) {
  const a = v.x
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("a"), "f32");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, UnknownMemberIsReported) {
  Fixture f(R"(
struct Vec3 {
  var x: float
}

fun take(v: Vec3) {
  const a = v.z
}
)");
  WOLANG_EXPECT(f.HasCode("E0112"));
  WOLANG_EXPECT(f.Messages().find("has no member named `z`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, MethodsSeeTheirOwnFields) {
  Fixture f(R"(
class Pawn {
  var health: float = 100.0

  fun Hurt(amount: float) {
    health = health - amount
    const left = self.health
  }
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("left"), "f32");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AssigningAnImmutableFieldIsReported) {
  Fixture f(R"(
class Pawn {
  const health: float = 100.0

  fun Hurt() {
    health = 1.0
  }
}
)");
  WOLANG_EXPECT(f.HasCode("E0105"));
}

WOLANG_TEST(TypeCheck, MethodCallsCheckTheirArguments) {
  Fixture f(R"(
class Pawn {
  fun Hurt(amount: float) {}
}

fun take(p: Pawn) {
  p.Hurt(true)
}
)");
  WOLANG_EXPECT(f.HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, OptionalMemberAccessNeedsTheSafeOperator) {
  Fixture f(R"(
struct Vec3 {
  var x: float
}

fun take(v: Vec3?) {
  const bad = v.x
  const good = v?.x
}
)");
  WOLANG_EXPECT(f.HasCode("E0104"));
  // Reaching through `?.` leaves the result optional.
  WOLANG_EXPECT_EQ(f.TypeOfInit("good"), "f32?");
}

// ---------------------------------------------------------- signatures -----

WOLANG_TEST(TypeCheck, CallsCheckAgainstTheDeclaredSignature) {
  Fixture f(R"(
fun Damage(base: float, mult: float) -> float => base

fun use() {
  const a = Damage(1.0, 2.0)
  const b = Damage(1.0)
  const c = Damage(1.0, true)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("a"), "f32");
  WOLANG_EXPECT(f.HasCode("E0102"));
  WOLANG_EXPECT(f.HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, ConstructorsFillFieldsInOrder) {
  Fixture f(R"(
struct Vec3 {
  var x: float
  var y: float
}

fun use() {
  const a = Vec3(1.0, 2.0)
  const b = Vec3()
  const c = Vec3(1.0)
  const d = Vec3(1.0, true)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("a"), "Vec3");
  // No arguments asks for the default value, which is always allowed.
  WOLANG_EXPECT_EQ(f.TypeOfInit("b"), "Vec3");
  WOLANG_EXPECT(f.HasCode("E0102"));
  WOLANG_EXPECT(f.HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, DimensionNamesAreTypes) {
  Fixture f(R"(
dimension Length
dimension Time
dimension Velocity = Length / Time

unit m: Length
unit s: Time

fun Travel(distance: Length, speed: Velocity) -> Time {
  return distance / speed
}

fun use() {
  const eta = Travel(3m, 2m / 1s)
  const bad = Travel(3s, 2m / 1s)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("eta"), "Time");
  WOLANG_EXPECT(f.HasCode("E0101"));
  WOLANG_EXPECT(f.Messages().find("expected `Length`, found `Time`") !=
                std::string::npos);
}


// ------------------------------------------------------ exhaustiveness -----

WOLANG_TEST(TypeCheck, EnumMatchMustCoverEveryVariant) {
  Fixture f(R"(
enum Shape {
  Circle(radius: float)
  Square(side: float)
  Point
}

fun area(s: Shape) -> float {
  return match s {
    Circle(r) => r
    Point => 0.0
  }
}
)");
  WOLANG_EXPECT(f.HasCode("E0201"));
  // The uncovered variant is named, which is the whole value of the check.
  WOLANG_EXPECT(f.Messages().find("Shape") != std::string::npos);
}

WOLANG_TEST(TypeCheck, CoveringEveryVariantIsExhaustive) {
  Fixture f(R"(
enum Shape {
  Circle(radius: float)
  Point
}

fun area(s: Shape) -> float {
  return match s {
    Circle(r) => r
    Point => 0.0
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, BoolMatchNeedsBothValues) {
  Fixture f("fun pick(b: bool) -> int {\n"
            "  return match b {\n    true => 1\n  }\n}\n");
  WOLANG_EXPECT(f.HasCode("E0201"));
}

WOLANG_TEST(TypeCheck, OptionalMatchNeedsNullAndPresent) {
  Fixture f(R"(
struct Vec3 { var x: float }

fun pick(v: Vec3?) -> int {
  return match v {
    null => 0
    x? => 1
  }
}

fun half(v: Vec3?) -> int {
  return match v {
    null => 0
  }
}
)");
  WOLANG_EXPECT(f.HasCode("E0201"));
  // One message, for `half` only.
  WOLANG_EXPECT_EQ(f.Codes(), "E0201");
}

WOLANG_TEST(TypeCheck, AnIntegerMatchNeedsACatchAll) {
  Fixture f("fun pick(n: int) -> int {\n"
            "  return match n {\n    0 => 1\n    1 => 2\n  }\n}\n");
  WOLANG_EXPECT(f.HasCode("E0201"));

  Fixture ok("fun pick(n: int) -> int {\n"
             "  return match n {\n    0 => 1\n    _ => 2\n  }\n}\n");
  WOLANG_EXPECT_EQ(ok.Codes(), "");
}

WOLANG_TEST(TypeCheck, AGuardedArmCoversNothing) {
  // A guard may be false, so the arm cannot be counted on.
  Fixture f("fun pick(b: bool) -> int {\n"
            "  return match b {\n    true => 1\n"
            "    false if b => 2\n  }\n}\n");
  WOLANG_EXPECT(f.HasCode("E0201"));
}


// ------------------------------------------------- generics and abilities -----

WOLANG_TEST(TypeCheck, GenericCallsInferTheirParameters) {
  Fixture f(R"(
fun First<T>(items: []T) -> T {
  return items[0]
}

fun use(xs: []int, ys: []float) {
  const a = First(xs)
  const b = First(ys)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("a"), "s64");
  WOLANG_EXPECT_EQ(f.TypeOfInit("b"), "f32");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, TakesTheTypeArgumentsACallWrites) {
  // Inference reads the arguments, so a signature that mentions its
  // parameter only in its result has nothing to be inferred from --
  // `SoaNew<Particle>(4)` is the whole reason the turbofish exists. Before
  // this, the call's type stayed `soa []T` and every field of the storage
  // was laid out against a type parameter.
  Fixture f(R"(
fun Empty<T>() -> []T {
  return []
}

unsafe fun Zero<T>(count: int) -> soa []T

fun use() uses unsafe {
  const a = Empty<int>()
  const b = Empty<float>()
  const c = Zero<float>(4)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("a"), "[]s64");
  WOLANG_EXPECT_EQ(f.TypeOfInit("b"), "[]f32");
  WOLANG_EXPECT_EQ(f.TypeOfInit("c"), "soa []f32");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, ConstructorsBindTheirTypeArguments) {
  Fixture f(R"(
struct Box<T> {
  var item: T
}

class Holder<T> {
  var item: T
  var count: int = 0
}

fun use() {
  const written = Box<int>(1)
  const inferred = Box(2.5)
  const handle = Holder<int>(3, 0)
  const field = inferred.item
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("written"), "Box<s64>");
  WOLANG_EXPECT_EQ(f.TypeOfInit("inferred"), "Box<f32>");
  WOLANG_EXPECT_EQ(f.TypeOfInit("handle"), "Holder<s64>");
  WOLANG_EXPECT_EQ(f.TypeOfInit("field"), "f32");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AGenericTypeWithNothingToBindItIsAnError) {
  // No field takes a `T` and the call wrote no argument, so there is
  // nothing to go on -- and a type parameter that reaches a layout is a
  // field of size zero rather than a diagnostic.
  Fixture f(R"(
struct Marker<T> {
  var tag: int = 0
}

fun use() {
  const m = Marker(1)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0126");
}

WOLANG_TEST(TypeCheck, InferenceAcrossSeveralParameters) {
  Fixture f(R"(
fun Pair<A, B>(a: A, b: B) -> (A, B) {
  return (a, b)
}

fun use() {
  const p = Pair(1, true)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("p"), "(s64, bool)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnArgumentThatCannotMatchIsReported) {
  Fixture f(R"(
fun First<T>(items: []T) -> T {
  return items[0]
}

fun use() {
  const a = First(1)
}
)");
  WOLANG_EXPECT(f.HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, InferredResultStillMeetsItsExpectation) {
  Fixture f(R"(
fun First<T>(items: []T) -> T {
  return items[0]
}

fun use(xs: []int) {
  const a: bool = First(xs)
}
)");
  WOLANG_EXPECT(f.HasCode("E0101"));
  WOLANG_EXPECT(f.Messages().find("expected `bool`, found `s64`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, ExtendDeclaresNoNameOfItsOwn) {
  // `extend Pawn` is named after the type it extends, so binding it would
  // collide with the declaration.
  Fixture f(R"(
struct Pawn {
  var health: float
}

extend Pawn {
  fun Heal() {}
}

fun use(p: Pawn) {
  p.Heal()
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, MissingAbilityMethodIsReported) {
  Fixture f(R"(
ability Damageable {
  fun TakeDamage(amount: float)
  fun IsAlive() -> bool
}

struct Pawn {
  var health: float
}

extend Pawn as Damageable {
  fun TakeDamage(amount: float) {}
}
)");
  WOLANG_EXPECT(f.HasCode("E0402"));
  WOLANG_EXPECT(f.Messages().find("does not implement `IsAlive`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, AnAbilitySatisfiedByAnotherExtensionIsAccepted) {
  // The methods may come from anywhere, not only from the block that names
  // the ability.
  Fixture f(R"(
ability Damageable {
  fun TakeDamage(amount: float)
  fun IsAlive() -> bool
}

struct Pawn {
  var health: float

  fun IsAlive() -> bool => true
}

extend Pawn as Damageable {
  fun TakeDamage(amount: float) {}
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}


// -------------------------------------------------------- flow typing -----

WOLANG_TEST(TypeCheck, AnExplicitNullCheckNarrowsTheName) {
  Fixture f(R"(
struct Pawn { var health: float }

fun take(p: Pawn?) -> float {
  if p != null {
    return p.health
  }
  return 0.0
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, NarrowingEndsWithTheBranch) {
  Fixture f(R"(
struct Pawn { var health: float }

fun take(p: Pawn?) -> float {
  if p != null {
    return p.health
  }
  return p.health
}
)");
  // Exactly one complaint, about the access after the branch.
  WOLANG_EXPECT_EQ(f.Codes(), "E0104");
}

WOLANG_TEST(TypeCheck, NullComparesInEitherOrder) {
  Fixture f(R"(
struct Pawn { var health: float }

fun take(p: Pawn?) -> float {
  if null != p {
    return p.health
  }
  return 0.0
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}


// --------------------------------------------- inheritance and bounds -----

WOLANG_TEST(TypeCheck, AnOverrideThatReplacesNothingIsReported) {
  Fixture f(R"(
class Actor {
  virtual fun Tick(dt: float) {}
}

class Pawn : Actor {
  override fun Missing() {}
}
)");
  WOLANG_EXPECT(f.HasCode("E0401"));
  WOLANG_EXPECT(f.Messages().find("replaces no base method") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, HidingABaseMethodNeedsOverride) {
  Fixture f(R"(
class Actor {
  virtual fun Tick(dt: float) {}
}

class Pawn : Actor {
  fun Tick(dt: float) {}
}
)");
  WOLANG_EXPECT(f.HasCode("E0401"));
  WOLANG_EXPECT(f.Messages().find("write `override`") != std::string::npos);
}

WOLANG_TEST(TypeCheck, OverridingANonVirtualMethodIsReported) {
  Fixture f(R"(
class Actor {
  fun Name() -> text => "actor"
}

class Pawn : Actor {
  override fun Name() -> text => "pawn"
}
)");
  WOLANG_EXPECT(f.HasCode("E0401"));
  WOLANG_EXPECT(f.Messages().find("not `virtual`") != std::string::npos);
}

WOLANG_TEST(TypeCheck, AProperOverrideIsAccepted) {
  Fixture f(R"(
class Actor {
  virtual fun Tick(dt: float) {}
}

class Pawn : Actor {
  override fun Tick(dt: float) {}
}

class Turret : Pawn {
  override fun Tick(dt: float) {}
}
)");
  // Including through two levels: an override is itself overridable.
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AMethodOnATypeWithNoBaseIsNotAnOverride) {
  Fixture f("struct Vec3 {\n  fun Length() -> float => 0.0\n}\n");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, ABoundIsCheckedAtTheCallSite) {
  Fixture f(R"(
ability Damageable {
  fun TakeDamage(amount: float)
}

struct Pawn { var health: float }
struct Rock {}

extend Pawn as Damageable {
  fun TakeDamage(amount: float) {}
}

fun Hurt<T: Damageable>(victim: T) {}

fun use(p: Pawn, r: Rock) {
  Hurt(p)
  Hurt(r)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0403");
  WOLANG_EXPECT(f.Messages().find("`Rock` does not satisfy `Damageable`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, ABaseClassSatisfiesABound) {
  // Conformance is inherited: a bound naming a base class accepts a derived
  // one, and an ability implemented on the base accepts it too.
  Fixture f(R"(
ability Damageable {
  fun TakeDamage(amount: float)
}

class Actor {}
class Pawn : Actor {}

extend Actor as Damageable {
  fun TakeDamage(amount: float) {}
}

fun Spawn<T: Actor>(what: T) {}
fun Hurt<T: Damageable>(victim: T) {}

fun use(p: Pawn) {
  Spawn(p)
  Hurt(p)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}


// ----------------------------------------------------------- warnings -----

WOLANG_TEST(TypeCheck, ReportsAnUnusedLocal) {
  Fixture f(R"(
fun f() -> int {
  const a = 1
  const b = 2
  return a
}
)");
  WOLANG_EXPECT_EQ(f.Warnings(), "W0101");
  WOLANG_EXPECT(f.Messages().find("`b` is never used") != std::string::npos);
}

WOLANG_TEST(TypeCheck, AnUnusedLocalCarriesItsFix) {
  // The warning says what to do about it; this is the compiler also
  // saying *where*, as an edit an editor can apply (D191).
  Fixture f(R"(
fun g() -> int {
  const kept = 1
  const dropped = 2
  return kept
}
)");
  WOLANG_EXPECT_EQ(f.Warnings(), "W0101");
  const std::vector<Diagnostic>* diagnostics = f.db().Diagnostics(f.file());
  WOLANG_ASSERT_NE(diagnostics, nullptr);
  bool found = false;
  for (const Diagnostic& diagnostic : *diagnostics) {
    if (diagnostic.code != "W0101" || diagnostic.fixes.empty()) {
      continue;
    }
    found = true;
    const Fix& fix = diagnostic.fixes[0];
    WOLANG_EXPECT_EQ(fix.message, "prefix with `_`");
    WOLANG_ASSERT_EQ(fix.edits.size(), size_t{1});
    WOLANG_EXPECT_EQ(fix.edits[0].new_text, "_");
    // An insertion at the name, not a replacement of it.
    WOLANG_EXPECT_EQ(fix.edits[0].span.begin, fix.edits[0].span.end);
    WOLANG_EXPECT_EQ(fix.edits[0].span.begin,
                     diagnostic.PrimarySpan().begin);
  }
  WOLANG_EXPECT(found);
}

WOLANG_TEST(TypeCheck, AnUnderscorePrefixOptsOut) {
  Fixture f(R"(
fun f(_unused: int, used: int) -> int {
  const _ignored = 1
  return used
}
)");
  WOLANG_EXPECT_EQ(f.Warnings(), "");
}

WOLANG_TEST(TypeCheck, ADeclarationWithNoBodyHasNoUnusedParameters) {
  // An ability method has nowhere to use its parameters.
  Fixture f(R"(
ability Damageable {
  fun TakeDamage(amount: float, from: int)
}
)");
  WOLANG_EXPECT_EQ(f.Warnings(), "");
}

WOLANG_TEST(TypeCheck, ReportsUnreachableCode) {
  Fixture f(R"(
fun f() -> int {
  return 1
  const a = 2
  const b = 3
}
)");
  // One message for the block, not one per statement after the return.
  WOLANG_EXPECT(f.Warnings().find("W0102") != std::string::npos);
  WOLANG_EXPECT_EQ(f.CodesOf(Severity::kWarning).find("W0102"), size_t{0});
  WOLANG_EXPECT(f.Messages().find("never reached") != std::string::npos);
}

WOLANG_TEST(TypeCheck, CodeAfterABreakIsUnreachable) {
  Fixture f(R"(
fun f() {
  loop {
    break
    const _a = 1
  }
}
)");
  WOLANG_EXPECT(f.Warnings().find("W0102") != std::string::npos);
}

WOLANG_TEST(TypeCheck, ALastStatementIsNotUnreachable) {
  Fixture f("fun f() -> int {\n  return 1\n}\n");
  WOLANG_EXPECT_EQ(f.Warnings(), "");
}

// --------------------------------------------- references are second class -

WOLANG_TEST(TypeCheck, AFieldCannotHoldAReference) {
  Fixture f(R"(
struct Holder {
  var r: &int
}
)");
  WOLANG_EXPECT(f.HasCode("E0120"));
  WOLANG_EXPECT(f.Messages().find("would outlive") != std::string::npos);
}

WOLANG_TEST(TypeCheck, AnExternClassIsOnlyMethods) {
  Fixture fine(R"(
extern class Actor {
  fun Health() -> s32
}

fun Read(actor_ref: Actor) -> s32 uses unsafe {
  return actor_ref.Health()
}
)");
  WOLANG_EXPECT_EQ(fine.Codes(), "");

  Fixture made(R"(
extern class Actor {
  fun Health() -> s32
}

fun Make() -> Actor {
  return Actor()
}
)");
  WOLANG_EXPECT(made.HasCode("E0139"));

  Fixture stored(R"(
extern class Actor {
  var health: int
}
)");
  WOLANG_EXPECT(stored.HasCode("E0139"));
}

WOLANG_TEST(TypeCheck, AScriptClassMayNotDeriveFromAHostClass) {
  Fixture derived(R"(
extern class Actor {
  fun Health() -> s32
}

class Pawn : Actor {
  var score: int = 0
}
)");
  WOLANG_EXPECT_EQ(derived.Codes(), "E0164");

  Fixture host(R"(
extern class Actor {
  fun Health() -> s32
}

extern class Pawn : Actor {
  fun Score() -> s32
}
)");
  WOLANG_EXPECT(!host.HasCode("E0164"));

  Fixture script(R"(
class Actor {
  var health: int = 0
}

class Pawn : Actor {
  var score: int = 0
}
)");
  WOLANG_EXPECT_EQ(script.Codes(), "");
}

WOLANG_TEST(TypeCheck, AModuleLevelWeakVarHoldsAnOptionalClass) {
  Fixture fine(R"(
class Node {
  var id: int = 0
}

weak var current: Node? = null

fun Set(node: Node) {
  current = node
}
)");
  WOLANG_EXPECT_EQ(fine.Codes(), "");

  Fixture host(R"(
extern class Actor {
  fun Health() -> s32
}

weak var pawn: Actor? = null
)");
  WOLANG_EXPECT_EQ(host.Codes(), "");

  Fixture not_optional(R"(
class Node {}

weak var current: Node = Node()
)");
  WOLANG_EXPECT(not_optional.HasCode("E0137"));

  Fixture not_class(R"(
weak var count: int? = null
)");
  WOLANG_EXPECT(not_class.HasCode("E0137"));

  Fixture local(R"(
class Node {}

fun F() {
  weak var current: Node? = null
}
)");
  WOLANG_EXPECT(local.HasCode("E0040"));
}

WOLANG_TEST(TypeCheck, AWeakFieldIsAnOptionalClassInAClass) {
  Fixture fine(R"(
class Node {
  weak var parent: Node? = null
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0137"));

  Fixture number(R"(
class Node {
  weak var count: int = 0
}
)");
  WOLANG_EXPECT(number.HasCode("E0137"));

  Fixture strong(R"(
class Node {
  weak var parent: Node = Node()
}
)");
  WOLANG_EXPECT(strong.HasCode("E0137"));

  Fixture value(R"(
class Node {
  var id: int = 0
}

struct Pair {
  weak var left: Node? = null
}
)");
  WOLANG_EXPECT(value.HasCode("E0137"));
}

WOLANG_TEST(TypeCheck, AnImplementationAnswersEveryAssociatedType) {
  Fixture fine(R"(
ability Walks {
  type Item
  fun Value() -> Item
}

class Counter {
  var at: int = 0
}

extend Counter as Walks {
  type Item = int
  fun Value() -> int => at
}

fun Read<W: Walks>(walker: W) -> W.Item {
  return walker.Value()
}

fun Use() -> int {
  return Read(Counter(3)) + 1
}
)");
  WOLANG_EXPECT_EQ(fine.Codes(), "");

  Fixture missing(R"(
ability Walks {
  type Item
  fun Value() -> Item
}

class Counter {
  var at: int = 0
}

extend Counter as Walks {
  fun Value() -> int => at
}
)");
  WOLANG_EXPECT(missing.HasCode("E0402"));
  WOLANG_EXPECT(missing.Messages().find("`Item`") != std::string::npos);

  Fixture abstract(R"(
ability Walks {
  type Item
  fun Value() -> Item
}

fun Add<W: Walks>(walker: W) -> int {
  return walker.Value() + 1
}
)");
  WOLANG_EXPECT(abstract.HasCode("E0108"));

  Fixture nothing(R"(
type Count
)");
  WOLANG_EXPECT(nothing.HasCode("E0138"));
}

WOLANG_TEST(TypeCheck, AParameterAndALocalMayHoldAReference) {
  Fixture f(R"(
fun f(a: &int) -> int {
  const b: &int = a
  return 0
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}


// --------------------------------------------------------------- allow -----

WOLANG_TEST(TypeCheck, AllowSilencesOneCodeForOneDeclaration) {
  Fixture f(R"(
@allow(W0101, reason = "kept for the ABI")
fun a() {
  const unused = 1
}

fun b() {
  const unused = 1
}
)");
  // Suppressed in `a`, still reported in `b`.
  WOLANG_EXPECT_EQ(f.Warnings(), "W0101");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AllowNeedsAReason) {
  Fixture f(R"(
@allow(W0101)
fun a() {
  const unused = 1
}
)");
  WOLANG_EXPECT(f.HasCode("E0070"));
  WOLANG_EXPECT(f.Messages().find("needs a reason") != std::string::npos);
  // The code is still honoured while the author fixes the attribute, so the
  // lint they named does not reappear on top of the complaint.
  WOLANG_EXPECT_EQ(f.Warnings(), "");
}

WOLANG_TEST(TypeCheck, AllowNeedsACode) {
  Fixture f("@allow(reason = \"nothing named\")\nfun a() {}\n");
  WOLANG_EXPECT(f.HasCode("E0070"));
  WOLANG_EXPECT(f.Messages().find("needs a diagnostic code") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, AllowTakesSeveralCodes) {
  Fixture f(R"(
@allow(W0101, W0102, reason = "generated code")
fun a() -> int {
  const unused = 1
  return 1
  const dead = 2
}
)");
  WOLANG_EXPECT_EQ(f.Warnings(), "");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AllowSilencesAnErrorToo) {
  Fixture f(R"(
@allow(E0112, reason = "the host adds this member at load time")
fun a(v: Vec3) {
  const x = v.whatever
}

struct Vec3 { var x: float }
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}


// --------------------------------------------------- slices and arrays -----

WOLANG_TEST(TypeCheck, AnArrayFitsWhereASliceIsWanted) {
  // A slice is a view of an array. Without this a slice has nothing to be a
  // view of.
  Fixture f(R"(
fun take(xs: []int) -> int => xs[0]

fun use() -> int {
  const arr: [3]int = [1, 2, 3]
  const view: []int = arr
  return take(arr) + view[0]
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, LayoutHasToAgreeForAView) {
  // A struct-of-arrays view of an array of structs is a different memory
  // shape, not a view of it.
  Fixture f(R"(
fun take(xs: soa []int) {}

fun use() {
  const arr: [2]int = [1, 2]
  take(arr)
}
)");
  WOLANG_EXPECT(f.HasCode("E0101"));
}

// ------------------------------------------------- recursive data types ----

WOLANG_TEST(TypeCheck, AValueTypeMayNotContainItself) {
  Fixture f(R"(
struct Direct {
  var next: Direct
}
)");
  WOLANG_EXPECT(f.HasCode("E0117"));
  WOLANG_EXPECT(f.Messages().find("has no size") != std::string::npos);
}

WOLANG_TEST(TypeCheck, ContainingItselfThroughAWrapperStillHasNoSize) {
  // An optional of a value type is that value plus a tag, and an array of it
  // is N of them: both are still stored inline.
  WOLANG_EXPECT(Fixture("struct S { var next: S? }").HasCode("E0117"));
  WOLANG_EXPECT(Fixture("struct S { var all: [4]S }").HasCode("E0117"));
  WOLANG_EXPECT(Fixture("struct S { var pair: (int, S) }").HasCode("E0117"));
}

WOLANG_TEST(TypeCheck, ARecursiveEnumPayloadHasNoSize) {
  WOLANG_EXPECT(Fixture("enum E {\n  X(e: E)\n  Y\n}\n").HasCode("E0117"));
  WOLANG_EXPECT(
      Fixture("enum E {\n  X(e: [4]E)\n  Y\n}\n").HasCode("E0117"));
  // Behind a slice the enum has a size.
  WOLANG_EXPECT_EQ(Fixture("enum E {\n  X(e: []E)\n  Y\n}\n").Codes(), "");
}

WOLANG_TEST(TypeCheck, AGenericValueTypeMayNotContainItself) {
  WOLANG_EXPECT(Fixture(
      "struct W<T> {\n  var t: T\n  var w: W<T>\n}\n").HasCode("E0117"));
  // The same declaration with a bigger argument is still itself.
  WOLANG_EXPECT(Fixture(
      "struct W<T> {\n  var w: W<W<T>>?\n}\n").HasCode("E0117"));
  WOLANG_EXPECT_EQ(Fixture(
      "struct W<T> {\n  var t: T\n  var w: []W<T>\n}\n").Codes(), "");
}

WOLANG_TEST(TypeCheck, AMutualCycleHasNoSizeEither) {
  Fixture f("struct A { var b: B }\nstruct B { var a: A }\n");
  WOLANG_EXPECT_EQ(f.Codes(), "E0117,E0117");
}

WOLANG_TEST(TypeCheck, OneIndirectionIsEnoughToHaveASize) {
  // A slice is a pointer and a length; a class is a reference to a heap
  // object. Neither stores the type inline.
  WOLANG_EXPECT_EQ(Fixture("struct S { var all: []S }").Codes(), "");
  WOLANG_EXPECT_EQ(Fixture("class C { var next: C }").Codes(), "");
}

WOLANG_TEST(TypeCheck, APlainEnumIsNotSelfContaining) {
  // A variant's own type is its enum, so counting one as a field would make
  // every enum look infinite.
  Fixture f(R"(
enum Shape {
  Circle(radius: float)
  Point
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AClassMayNotDeriveFromItself) {
  WOLANG_EXPECT(Fixture("class A : A {}").HasCode("E0118"));

  Fixture chain("class B : C {}\nclass C : B {}\n");
  WOLANG_EXPECT_EQ(chain.Codes(), "E0118,E0118");
}

// ---------------------------------------------------- generic members ------

WOLANG_TEST(TypeCheck, AGenericTypeParameterIsInScopeForItsMembers) {
  Fixture f(R"(
struct Holder<T> {
  var value: T
}

class Box<T> {
  var item: T

  fun Get() -> T => item
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AFieldOfAGenericInstanceIsSubstituted) {
  Fixture f(R"(
struct Holder<T> {
  var value: T
}

fun use(h: Holder<int>) {
  const good = h.value
  const bad: bool = h.value
  const missing = h.nothing
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("good"), "s64");
  WOLANG_EXPECT(f.HasCode("E0101"));
  WOLANG_EXPECT(f.HasCode("E0112"));
}


WOLANG_TEST(TypeCheck, ACyclicDimensionIsReported) {
  // Evaluation runs in declaration order, so a cycle would quietly produce
  // whichever answer that order happened to give.
  WOLANG_EXPECT(Fixture("dimension D = D").HasCode("E0118"));

  Fixture pair("dimension A = B\ndimension B = A\n");
  WOLANG_EXPECT(pair.HasCode("E0118"));

  // A real derived dimension still works.
  Fixture fine(
      "dimension L\ndimension T\ndimension V = L / T\n");
  WOLANG_EXPECT_EQ(fine.Codes(), "");
}


// --------------------------------------------------------- type aliases ----

WOLANG_TEST(TypeCheck, AnAliasIsTransparent) {
  // An alias is another spelling of its target, not a new type.
  Fixture f(R"(
type Count = int

fun f(n: Count) -> int => n

fun g() {
  const c: Count = 5
  const i: int = c
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnAliasChainSettles) {
  Fixture f(R"(
type Deep = Alias
type Alias = Count
type Count = int

fun f(d: Deep) -> int => d
)");
  // Declared out of order on purpose: the target of one alias is another
  // that has not been seen yet.
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, ACyclicAliasIsReported) {
  WOLANG_EXPECT(Fixture("type A = A").HasCode("E0118"));

  Fixture pair("type B = C\ntype C = B\n");
  WOLANG_EXPECT_EQ(pair.Codes(), "E0118,E0118");
}


WOLANG_TEST(TypeCheck, ADuplicateTypeParameterIsReported) {
  // Which of the two wins depends on a shadowing rule nobody should have to
  // know, so neither does.
  WOLANG_EXPECT(Fixture("fun f<T, T>(x: T) {}").HasCode("E0119"));
  WOLANG_EXPECT(
      Fixture("struct S<A, A> { var v: A }").HasCode("E0119"));
}

WOLANG_TEST(TypeCheck, ABoundMayNameAnEarlierParameter) {
  Fixture f(R"(
ability Show {
  fun Render()
}

fun take<A: Show, B: A>(a: A, b: B) {}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, AcceptsANumericConversion) {
  Fixture f(R"(
fun F(n: s32, x: f64) {
  const _a = n as s64
  const _b = n as f32
  const _c = x as u8
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, RejectsAConversionThatIsNotNumeric) {
  // `as` is the escape hatch D33 left when it removed implicit widening,
  // not a reinterpretation of arbitrary bits.
  Fixture f(R"(
struct Vec2 { var x: float }

fun F(s: text, v: Vec2, b: bool) {
  const _a = s as int
  const _b = v as float
  const _c = b as int
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0124,E0124,E0124");
}

WOLANG_TEST(TypeCheckTest, RejectsAVariableUsedBeforeItIsWritten) {
  // This is also what makes the two execution tiers agree: the interpreter
  // traps on reading a slot nothing wrote, and compiled code reads the zero
  // its prologue left. Making the program impossible settles it.
  Fixture f(R"(
fun F() -> int {
  var x: int
  return x
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0123");
}

WOLANG_TEST(TypeCheckTest, ReportsAVariableUsedBeforeWrittenOnce) {
  // Three uses of one unwritten variable are one mistake.
  Fixture f(R"(
fun F() -> int {
  var x: int
  return x + x + x
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0123");
}

WOLANG_TEST(TypeCheckTest, AcceptsAVariableWrittenOnEveryPath) {
  Fixture f(R"(
fun F(c: bool) -> int {
  var x: int
  if c {
    x = 1
  } else {
    x = 2
  }
  return x
}

fun G(c: bool) -> int {
  var y: int
  if c {
    return 0
  } else {
    y = 3
  }
  return y
}

fun H(c: bool) -> int {
  var z: int
  if c {
    return 0
  }
  z = 4
  return z
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, RejectsAVariableWrittenOnOnlyOnePath) {
  Fixture f(R"(
fun F(c: bool) -> int {
  var x: int
  if c {
    x = 1
  }
  return x
}

fun Loop(n: int) -> int {
  var y: int
  while n > 0 {
    y = n
  }
  return y
}
)");
  // A branch that may not run, and a loop body that may not run either.
  WOLANG_EXPECT_EQ(f.Codes(), "E0123,E0123");
}

WOLANG_TEST(TypeCheckTest, AVariableReadsItselfBeforeItIsWritten) {
  Fixture f(R"(
fun F() -> int {
  var x: int
  x = x + 1
  return x
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0123");
}

WOLANG_TEST(TypeCheckTest, AMatchHasToWriteOnEveryArm) {
  Fixture f(R"(
enum Shape {
  Circle
  Square
}

fun Sides(s: Shape) -> int {
  var n: int
  match s {
    Circle => n = 0
    Square => n = 4
  }
  return n
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, ADereferenceDoesNotTakeTheContextsType) {
  // The expectation reaches an operand only where the operator preserves
  // the type. Passing it through `*p` checked the pointer against what the
  // pointee has to be, and reported two errors for a correct program.
  Fixture f(R"(
extern "C" fun malloc(size: u64) -> *s32

fun Read(p: *s32) -> s32 {
  return *p
}

fun Write(p: *s32, n: s32) {
  *p = n
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, RejectsUseAfterMove) {
  // A `String` owns its bytes, so it cannot be in two places. Which use
  // moves and which borrows is the question "what did the context want":
  // `String` moves, `str` borrows (D15b).
  Fixture f(R"(
offer fun Print(message: text) uses io

fun Moved(n: int) uses io {
  const a = "value {n}"
  const b = a
  Print(b)
  Print(a)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0122");
}

WOLANG_TEST(TypeCheckTest, FormattingAnOwnedTextInALoopIsABorrow) {
  Fixture borrowed(R"(
fun Shout(n: int) -> int {
  const t = "value {n}"
  var total = 0
  for i in 0..3 {
    const line = "{t} {i}"
    total = total + 1
  }
  return total
}
)");
  WOLANG_EXPECT(!borrowed.HasCode("E0122"));

  Fixture moved(R"(
fun Shout(n: int) -> int {
  const t = "value {n}"
  var total = 0
  for i in 0..3 {
    const moved = t
    total = total + 1
  }
  return total
}
)");
  WOLANG_EXPECT(moved.HasCode("E0122"));
}

WOLANG_TEST(TypeCheckTest, ABorrowDoesNotMove) {
  Fixture f(R"(
offer fun Print(message: text) uses io

fun Borrowed(n: int) uses io {
  const a = "value {n}"
  Print(a)
  Print(a)
  Print(a)
}

fun Rewritten(n: int) uses io {
  var a = "value {n}"
  const b = a
  a = "other {n}"
  Print(a)
  Print(b)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, AMoveOnOnePathIsAMove) {
  // A value moved on one branch is gone at the join: the other branch
  // cannot know which way control came.
  Fixture f(R"(
offer fun Take(text: Text) uses io

fun Maybe(c: bool, n: int) uses io {
  const a = "value {n}"
  if c {
    Take(a)
  }
  Take(a)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0122");
}

WOLANG_TEST(TypeCheckTest, CopyingANumberIsNotAMove) {
  Fixture f(R"(
fun Twice(n: int) -> int {
  const a = n
  const b = a
  return a + b
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, AMoveInALoopBodyIsAMoveTwice) {
  // The second iteration moves what the first one already did, which is
  // the mistake worth reporting -- and the reason a loop body is walked
  // twice.
  Fixture f(R"(
offer fun Take(text: Text) uses io

fun InALoop(n: int) uses io {
  const a = "value {n}"
  var i = 0
  while i < n {
    Take(a)
    i = i + 1
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0122");
}

WOLANG_TEST(TypeCheckTest, RejectsAReferenceThatOutlivesWhatItPointsInto) {
  // A reference is second class (D8): it may be passed inwards and never
  // kept somewhere that outlives it. A `region` is one scope it can fail
  // to outlive; the call frame is the other.
  Fixture f(R"(
fun Returned() -> &int {
  var x = 1
  return &x
}

fun OutOfRegion() {
  var outer = &0
  arena {
    var inner = 2
    outer = &inner
  }
}

fun AsTheRegionsValue() -> int {
  const kept = arena {
    var inner = 2
    &inner
  }
  return *kept
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0121,E0121,E0121");
}

WOLANG_TEST(TypeCheckTest, AReferenceInsideItsOwnScopeIsFine) {
  Fixture f(R"(
fun Local() -> int {
  arena {
    var inner = 2
    const kept = &inner
    return *kept
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, RejectsEscapesThroughPathsCallsAndArrays) {
  // The root of a reference is found through fields, indices, calls that
  // return a reference or slice, and a local array used as a slice (D228).
  Fixture f(R"(
class C {
  var n: int = 0
}
fun Ret(a: &int) -> &int => a

fun ViaField() -> &int {
  const c = C(2)
  return &c.n
}

fun ViaIndex() -> &int {
  var a: [4]int
  return &a[1]
}

fun ViaCall() -> &int {
  var x = 1
  return Ret(&x)
}

fun ViaArray() -> []int {
  var a: [4]int
  return a
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0121,E0121,E0121,E0121");
}

WOLANG_TEST(TypeCheckTest, ReferencesToOutsideStorageMayBeReturned) {
  // Non-firing twin: a reference derived from a parameter, or a slice that
  // is passed through, points at the caller's storage.
  Fixture f(R"(
fun Ret(a: &int) -> &int => a
fun Through(a: &int) -> &int {
  return Ret(a)
}
fun Same(s: []int) -> []int {
  return s
}
fun Local() -> int {
  var x = 1
  return *Ret(&x)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, RejectsAnArenaObjectThatLeavesTheBlock) {
  // An object made in an arena is gone when the block ends (D228): it may
  // not be stored outside, returned, or given out as the block's value.
  Fixture f(R"(
class Leaf {
  var id: int = 0
}
class Box {
  var leaf: Leaf = Leaf(0)
}

var kept: Leaf? = null

fun ToGlobal() {
  arena {
    kept = Leaf(1)
  }
}

fun ToOuterLocal() {
  var outer: Leaf? = null
  arena {
    const inner = Leaf(2)
    outer = inner
  }
}

fun ToOuterField(b: Box) {
  arena {
    b.leaf = Leaf(3)
  }
}

fun Returned() -> Leaf {
  arena {
    const l = Leaf(4)
    return l
  }
}

fun AsValue() -> Leaf {
  return arena {
    Leaf(5)
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0142,E0142,E0142,E0142,E0142");
}

WOLANG_TEST(TypeCheckTest, AnArenaObjectInsideItsBlockIsFine) {
  // Non-firing twin: using it, and keeping an object made outside.
  Fixture f(R"(
class Leaf {
  var id: int = 0
}
class Box {
  var leaf: Leaf = Leaf(0)
}

fun Inside() -> int {
  const outside = Leaf(9)
  var total = 0
  arena {
    const a = Leaf(1)
    const b = Box(a)
    b.leaf = Leaf(2)
    total = a.id + b.leaf.id + outside.id
  }
  return total
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheckTest, OnlyATaskMaySuspend) {
  // `wait` is where a function suspends, and `task` is the signature's way
  // of saying it can. Keeping the word at the call site is what makes a
  // suspension point visible; putting `async` in the effect row is what
  // makes it checkable.
  Fixture f(R"(
task fun Fetch(n: int) -> int uses io

task fun Load(n: int) -> int uses io {
  const value = wait Fetch(n)
  return value + 1
}

fun Sync(n: int) -> int uses io {
  return wait Fetch(n)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0301");
}

WOLANG_TEST(TypeCheckTest, ATaskHasToBeWaitedForOrSpawned) {
  // Calling a task starts work rather than doing it, so a bare call is
  // almost always a mistake -- and the one that is hardest to see.
  Fixture f(R"(
task fun Fetch(n: int) -> int uses io

fun Forgot(n: int) -> int uses io {
  return Fetch(n)
}

task fun Waited(n: int) -> int uses io {
  return wait Fetch(n)
}

task fun Started(n: int) uses io {
  const _handle = spawn Fetch(n)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0302");
}

WOLANG_TEST(TypeCheck, AValueTypeMethodCannotWriteToItsReceiver) {
  // The receiver of a value type is a copy, so the write would be lost.
  // Reporting it is the whole point: before this wave it compiled to
  // nothing at all and the program quietly did the wrong thing.
  Fixture f(R"(
struct Point {
  var x: int = 0

  fun Shift(by: int) {
    x = x + by
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0125");
}

WOLANG_TEST(TypeCheck, AClassMethodMayWriteToItsReceiver) {
  // A class is a handle, so the receiver is the object and the write lands
  // on it. Which is also why a `const` handle may be written through: what
  // is constant is the handle, not what it points at.
  Fixture f(R"(
class Pawn {
  var health: int = 100

  fun Hurt(amount: int) {
    health = health - amount
  }
}

fun Use() {
  const p = Pawn(10)
  p.health = 5
  p.Hurt(1)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, WritingAFieldOfAValueNeedsVar) {
  // A struct is its value, so assigning to a field of it is assigning to
  // the binding -- and a `const` binding cannot be assigned to.
  Fixture f(R"(
struct Point {
  var x: int = 0
}

fun Use() {
  const p = Point(1)
  p.x = 2
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0105");
}

WOLANG_TEST(TypeCheckTest, DeinitCannotStoreSelf) {
  Fixture fires(R"(
var stash: Zombie? = null

class Zombie {
  var id: int = 0
  deinit {
    stash = self
  }
}
)");
  WOLANG_EXPECT(fires.HasCode("E0145"));

  Fixture fine(R"(
var seen: int = 0

class Zombie {
  var id: int = 0
  deinit {
    seen = self.id
  }
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0145"));
}

WOLANG_TEST(TypeCheckTest, MovingAPayloadOutOfAMatchMovesTheEnum) {
  Fixture fires(R"(
struct Res {
  var id: int
  deinit {
  }
}

enum Wrap {
  Empty
  Has(r: Res)
}

fun Take(r: Res) -> int => r.id

fun Twice() -> int {
  const w = Wrap.Has(Res(1))
  const a = match w {
    Wrap.Has(r) => Take(r),
    Wrap.Empty => 0,
  }
  const b = match w {
    Wrap.Has(r) => Take(r),
    Wrap.Empty => 0,
  }
  return a + b
}
)");
  WOLANG_EXPECT(fires.HasCode("E0122"));

  Fixture fine(R"(
struct Res {
  var id: int
  deinit {
  }
}

enum Wrap {
  Empty
  Has(r: Res)
}

fun Twice() -> int {
  const w = Wrap.Has(Res(1))
  const a = match w {
    Wrap.Has(r) => r.id,
    Wrap.Empty => 0,
  }
  const b = match w {
    Wrap.Has(r) => r.id,
    Wrap.Empty => 0,
  }
  return a + b
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0122"));
}

// A delay is a number of seconds that the generated `Tick(dt)` counts up
// to (D260); one that is not a number cannot be counted to (D243).
WOLANG_TEST(TypeCheck, AfterTakesANumberOfSeconds) {
  Fixture f(R"(
machine Door {
  state Closed { on Interact -> Open }
  state Open { on Interact after "soon" -> Closed }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0101");
  Fixture fine(R"(
machine Door {
  state Closed { on Interact -> Open }
  state Open { on Interact after 3.0 -> Closed }
}

fun Run(door: Door) -> bool {
  return door.Tick(0.5)
}
)");
  WOLANG_EXPECT_EQ(fine.Codes(), "");
}

WOLANG_TEST(TypeCheck, ATickOnAMachineWithoutAfterIsNotAMember) {
  Fixture f(R"(
machine Door {
  state Closed { on Interact -> Open }
  state Open { on Interact -> Closed }
}

fun Run(door: Door) -> bool {
  return door.Tick(0.5)
}
)");
  WOLANG_EXPECT(f.HasCode("E0112"));
}

// `[]T` is read-only and `[]mut T` is writable, as `*T` and `*mut T` are
// (D251).
WOLANG_TEST(TypeCheck, WritingAnElementOfAReadOnlySliceIsE0165) {
  Fixture f(R"(
fun Fill(xs: []int) {
  xs[0] = 1
}
)");
  WOLANG_EXPECT(f.HasCode("E0165"));
  Fixture compound(R"(
fun Bump(xs: []int) {
  xs[0] += 1
}
)");
  WOLANG_EXPECT(compound.HasCode("E0165"));
  Fixture field(R"(
struct P {
  var x: int
}

fun Move(ps: []P) {
  ps[0].x = 1
}
)");
  WOLANG_EXPECT(field.HasCode("E0165"));
  Fixture fine(R"(
fun Fill(xs: []mut int) {
  xs[0] = 1
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0165"));
  WOLANG_EXPECT_EQ(fine.Codes(), "");
}

WOLANG_TEST(TypeCheck, WritingThroughAHandleInASliceIsNotAWriteToTheSlice) {
  Fixture f(R"(
class Cell {
  var n: int
}

fun Fill(cells: []Cell) {
  cells[0].n = 1
}
)");
  WOLANG_EXPECT(!f.HasCode("E0165"));
}

WOLANG_TEST(TypeCheck, AWritableSliceFitsAReadOnlyOneAndNotTheOtherWay) {
  Fixture fine(R"(
fun Read(xs: []int) -> int {
  return xs[0]
}

fun Pass(xs: []mut int) -> int {
  return Read(xs)
}
)");
  WOLANG_EXPECT_EQ(fine.Codes(), "");
  Fixture f(R"(
fun Fill(xs: []mut int) {
  xs[0] = 1
}

fun Pass(xs: []int) {
  Fill(xs)
}
)");
  WOLANG_EXPECT(f.HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, AnArrayIsAViewOfEitherKind) {
  Fixture f(R"(
fun Fill(xs: []mut int) {
  xs[0] = 1
}

fun Read(xs: []int) -> int {
  return xs[0]
}

fun Run() -> int {
  var a = [1, 2, 3]
  Fill(a)
  return Read(a)
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, SelfIsTheMachineInItsBlocks) {
  Fixture f(R"(
machine Door {
  state Closed {
    enter { const again: Self = self }
    on Interact -> Open
  }
  state Open { on Interact -> Closed }
}
)");
  WOLANG_EXPECT(!f.HasCode("E0156"));
}

WOLANG_TEST(TypeCheckTest, MovingAnOwningFieldOutOfItsOwnerIsE0149) {
  // `o` drops `r` when it ends; handing `r` on as well would drop it twice
  // (D233).
  Fixture fires(R"(
struct Res {
  var id: int
  deinit {
  }
}

struct Outer {
  var r: Res
}

fun Take(r: Res) -> int => r.id

fun F() -> int {
  const o = Outer(Res(1))
  return Take(o.r)
}
)");
  WOLANG_EXPECT(fires.HasCode("E0149"));

  Fixture fine(R"(
struct Res {
  var id: int
  deinit {
  }
}

struct Outer {
  var r: Res
}

fun F() -> int {
  const o = Outer(Res(1))
  return o.r.id
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0149"));
}

WOLANG_TEST(TypeCheckTest, MovingAParameterTwiceIsE0122) {
  // A by-value parameter that owns something is a flow local too (D233).
  Fixture fires(R"(
struct Res {
  var id: int
  deinit {
  }
}

fun Take(r: Res) -> int => r.id

fun F(r: Res) -> int {
  const a = Take(r)
  const b = Take(r)
  return a + b
}
)");
  WOLANG_EXPECT(fires.HasCode("E0122"));

  Fixture fine(R"(
struct Res {
  var id: int
  deinit {
  }
}

fun Take(r: Res) -> int => r.id

fun F(r: Res) -> int {
  const a = r.id
  return a + Take(r)
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0122"));
}

WOLANG_TEST(TypeCheckTest, AClosureCannotMoveWhatItCaptured) {
  // The capture is a copy of a view of the enclosing function's value,
  // which still drops it; moving it on from the body drops it twice (D233).
  Fixture fires(R"(
struct Res {
  var id: int
  deinit {
  }
}

fun Take(r: Res) -> int => r.id

fun F() -> int {
  const r = Res(1)
  const f = || Take(r)
  return f()
}
)");
  WOLANG_EXPECT(fires.HasCode("E0149"));

  Fixture fine(R"(
struct Res {
  var id: int
  deinit {
  }
}

fun Take(r: Res) -> int => r.id

fun F() -> int {
  const r = Res(1)
  const f = || r.id
  const n = f()
  return n + Take(r)
}
)");
  WOLANG_EXPECT(!fine.HasCode("E0149"));
  WOLANG_EXPECT(!fine.HasCode("E0122"));
}

WOLANG_TEST(TypeCheckTest, AnEnumHoldingAnOwningValueIsMovedAsAWhole) {
  Fixture fires(R"(
struct Res {
  var id: int
  deinit {
  }
}

enum Wrap {
  None
  Has(r: Res)
}

fun F() {
  const r = Res(1)
  const w = Wrap.Has(r)
  const _c = w
  const _d = w
}
)");
  WOLANG_EXPECT(fires.HasCode("E0122"));
}

// D239: an operator is an ability, and `Self` is the type that implements it.
// The abilities are declared inline because a sema test has no imports.
static constexpr const char kOpsPrelude[] = R"(
@Operator
ability Add {
  fun Plus(other: Self) -> Self
}

@Operator
ability Eq {
  fun Equals(other: &Self) -> bool
}

@Operator
ability Neg {
  fun Negate() -> Self
}

ability Hash {
  fun Hash() -> u64
}

struct Vec3 {
  var x: float
  var y: float
}
)";

WOLANG_TEST(TypeCheck, AnOperatorOnAStructCallsItsAbilityMethod) {
  Fixture f(std::string(kOpsPrelude) + R"(
extend Vec3 as Add {
  fun Plus(other: Self) -> Self => Vec3(x + other.x, y + other.y)
}

extend Vec3 as Neg {
  fun Negate() -> Self => Vec3(-x, -y)
}

extend Vec3 as Eq {
  fun Equals(other: &Self) -> bool => x == other.x
}

fun Use(a: Vec3, b: Vec3) -> Vec3 {
  var c = a + b
  c += b
  const same = a == b
  const other = a != b
  return -c
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnOperatorRecordsTheCallForLowering) {
  Fixture f(std::string(kOpsPrelude) + R"(
extend Vec3 as Add {
  fun Plus(other: Self) -> Self => Vec3(x + other.x, y + other.y)
}

fun Use(a: Vec3, b: Vec3) -> Vec3 => a + b
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
  const CheckedFile* checked = f.db().Checked(f.file());
  WOLANG_ASSERT(checked != nullptr);
  size_t recorded = 0;
  for (const TypedBody& body : checked->items) {
    for (const TypedBody::OperatorCall& call : body.operator_calls) {
      recorded += call.kind == OperatorKind::kPlus && body.OperatorOf(call.expr) != nullptr;
    }
  }
  WOLANG_EXPECT_EQ(recorded, size_t{1});
}

WOLANG_TEST(TypeCheck, AnOperatorWorksThroughABound) {
  Fixture f(std::string(kOpsPrelude) + R"(
fun Twice<T: Add>(a: T) -> T => a + a

fun Same<T: Eq>(a: T, b: T) -> bool => a == b
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, NumbersMeetAnOperatorBound) {
  Fixture f(std::string(kOpsPrelude) + R"(
fun Twice<T: Add>(a: T) -> T => a + a

fun Use() -> int => Twice(4)

fun Cmp() -> bool => Same(1.5, 2.5)

fun Same<T: Eq>(a: T, b: T) -> bool => a == b
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnOperatorNeedsItsAbilityNotJustTheMethod) {
  Fixture f(std::string(kOpsPrelude) + R"(
extend Vec3 {
  fun Plus(other: Vec3) -> Vec3 => Vec3(x + other.x, y + other.y)
}

fun Use(a: Vec3, b: Vec3) -> Vec3 => a + b
)");
  WOLANG_EXPECT(f.HasCode("E0108"));
}

WOLANG_TEST(TypeCheck, SelfOutsideATypeIsReported) {
  Fixture fires(R"(
fun Free(x: Self) -> int => 1
)");
  WOLANG_EXPECT(fires.HasCode("E0156"));

  Fixture twin(R"(
struct Box {
  var n: int
}

extend Box {
  fun Copy() -> Self => Box(n)
}

struct Cell {
  var n: int
  fun Next() -> Self => Cell(n + 1)
}
)");
  WOLANG_EXPECT_EQ(twin.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnAbilityThatTakesSelfIsNotAValueType) {
  Fixture fires(std::string(kOpsPrelude) + R"(
fun Make(a: Vec3) {
  const e: Eq = a
}
)");
  WOLANG_EXPECT(fires.HasCode("E0157"));

  Fixture param(std::string(kOpsPrelude) + R"(
fun Take(e: Eq) {}
)");
  WOLANG_EXPECT(param.HasCode("E0157"));

  Fixture twin(std::string(kOpsPrelude) + R"(
fun Bound<T: Eq>(a: T, b: T) -> bool => a == b

fun Dynamic(h: Hash) {}
)");
  WOLANG_EXPECT(!twin.HasCode("E0157"));
}

WOLANG_TEST(TypeCheck, AnImplementationHasToHaveTheShapeOfSelf) {
  Fixture fires(std::string(kOpsPrelude) + R"(
extend Vec3 as Add {
  fun Plus(other: float) -> Self => Vec3(x, y)
}
)");
  WOLANG_EXPECT(fires.HasCode("E0402"));

  Fixture twin(std::string(kOpsPrelude) + R"(
extend Vec3 as Add {
  fun Plus(other: Vec3) -> Vec3 => Vec3(x + other.x, y + other.y)
}
)");
  WOLANG_EXPECT_EQ(twin.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnOperatorAbilityHasOneOperatorShapedMethod) {
  Fixture fires(R"(
@Operator
ability Odd {
  fun Frob(other: Self) -> Self
}
)");
  WOLANG_EXPECT(fires.HasCode("E0160"));

  Fixture wrong_shape(R"(
@Operator
ability Odd {
  fun Plus(other: int) -> int
}
)");
  WOLANG_EXPECT(wrong_shape.HasCode("E0160"));

  Fixture twin{std::string(kOpsPrelude)};
  WOLANG_EXPECT_EQ(twin.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnOfferedFunctionMustStateItsErrors) {
  Fixture open("error A\noffer fun F() -> int! {\n  return 1\n}\n");
  WOLANG_EXPECT(open.HasCode("E0150"));

  Fixture stated("error A\noffer fun F() -> int!(A) {\n  return A\n}\n");
  WOLANG_EXPECT(!stated.HasCode("E0150"));

  Fixture private_open("error A\nfun F() -> int! {\n  return 1\n}\n");
  WOLANG_EXPECT(!private_open.HasCode("E0150"));
}

WOLANG_TEST(TypeCheck, AReturnedErrorMustBeInTheStatedSet) {
  Fixture outside("error A, B\nfun F() -> int!(A) {\n  return B\n}\n");
  WOLANG_EXPECT(outside.HasCode("E0151"));

  Fixture inside("error A, B\nfun F() -> int!(A) {\n  return A\n}\n");
  WOLANG_EXPECT_EQ(inside.Codes(), "");
}

WOLANG_TEST(TypeCheck, TryChecksTheStatedSetOfTheCaller) {
  Fixture open_callee(
      "error A\n"
      "fun G() -> int! {\n  return A\n}\n"
      "fun F() -> int!(A) {\n  return G()?\n}\n");
  WOLANG_EXPECT(open_callee.HasCode("E0151"));

  Fixture stated_callee(
      "error A\n"
      "fun G() -> int!A {\n  return A\n}\n"
      "fun F() -> int!(A) {\n  return G()?\n}\n");
  WOLANG_EXPECT(!stated_callee.HasCode("E0151"));
}

WOLANG_TEST(TypeCheck, AnErrorWithDataNeedsASetThatNamesIt) {
  Fixture open("error P(x: int)\nfun F() -> int! {\n  return P(1)\n}\n");
  WOLANG_EXPECT(open.HasCode("E0152"));

  Fixture stated("error P(x: int)\nfun F() -> int!(P) {\n  return P(1)\n}\n");
  WOLANG_EXPECT(!stated.HasCode("E0152"));
  WOLANG_EXPECT_EQ(stated.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnErrorWithDataIsBuiltWithItsArguments) {
  Fixture bare("error P(x: int)\nfun F() -> int!(P) {\n  const e = P\n  return 1\n}\n");
  WOLANG_EXPECT(bare.HasCode("E0101"));

  Fixture wrong("error P(x: int)\nfun F() -> int!(P) {\n  return P(true)\n}\n");
  WOLANG_EXPECT(wrong.HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, ANamedErrorMustExist) {
  Fixture unknown("fun F() -> int!(Nope) {\n  return 1\n}\n");
  WOLANG_EXPECT(unknown.HasCode("E0110"));

  Fixture known("error A\nfun F() -> int!(A) {\n  return 1\n}\n");
  WOLANG_EXPECT(!known.HasCode("E0110"));
}

WOLANG_TEST(TypeCheck, MatchOnAResultMustCoverItsErrors) {
  Fixture open(
      "error Small\n"
      "fun Root(x: int) -> int! {\n  return Small\n}\n"
      "fun Use() -> int {\n"
      "  return match Root(1) {\n    Ok(v) => v\n    Small => 0\n  }\n"
      "}\n");
  WOLANG_EXPECT(open.HasCode("E0201"));

  Fixture open_with_catch_all(
      "error Small\n"
      "fun Root(x: int) -> int! {\n  return Small\n}\n"
      "fun Use() -> int {\n"
      "  return match Root(1) {\n    Ok(v) => v\n    _ => 0\n  }\n"
      "}\n");
  WOLANG_EXPECT(!open_with_catch_all.HasCode("E0201"));

  Fixture stated_missing(
      "error Small, Big\n"
      "fun Root(x: int) -> int!(Small, Big) {\n  return Small\n}\n"
      "fun Use() -> int {\n"
      "  return match Root(1) {\n    Ok(v) => v\n    Small => 0\n  }\n"
      "}\n");
  WOLANG_EXPECT(stated_missing.HasCode("E0201"));

  Fixture stated_covered(
      "error Small, Big\n"
      "fun Root(x: int) -> int!(Small, Big) {\n  return Small\n}\n"
      "fun Use() -> int {\n"
      "  return match Root(1) {\n    Ok(v) => v\n    Small => 0\n    Big => 1\n  }\n"
      "}\n");
  WOLANG_EXPECT(!stated_covered.HasCode("E0201"));
}

WOLANG_TEST(TypeCheck, APayloadBindsInAMatchArm) {
  Fixture f(
      "error P(x: int, why: text)\n"
      "fun Make() -> int!(P) {\n  return P(7, \"no\")\n}\n"
      "fun Use() -> int {\n"
      "  return match Make() {\n    Ok(v) => v\n    P(x, why) => x\n  }\n"
      "}\n");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, UsesNamesAnEffect) {
  WOLANG_EXPECT(Fixture("fun F() uses alloc {}").HasCode("E0158"));
  WOLANG_EXPECT(Fixture("fun F() uses Nope {}").HasCode("E0158"));
  WOLANG_EXPECT(!Fixture("fun F() uses io {}").HasCode("E0158"));
  WOLANG_EXPECT(!Fixture("effect Telemetry\nfun F() uses Telemetry {}").HasCode("E0158"));
}

}  // namespace
}  // namespace wolang
