// Copyright 2026 The Wolang Authors. All rights reserved.

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

  std::string Codes() {
    std::string result;
    const std::vector<Diagnostic>* diagnostics = db_.Diagnostics(file_);
    if (diagnostics == nullptr) {
      return result;
    }
    for (const Diagnostic& diagnostic : *diagnostics) {
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
      "  let a = 1\n"
      "  let b: u8 = 200\n"
      "  let c: i64 = 1\n"
      "  let d: f64 = 2\n"
      "  let e = 0xFFFFFFFFFFFF\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  // Without an expectation a literal settles on `int`.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "i32");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "u8");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "i64");
  // An integer literal in a float context is fine.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("d"), "f64");
  // Arbitrary precision until used, so no suffix is needed.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("e"), "i32");
}

WOLANG_TEST(TypeCheck, ReportsALiteralThatDoesNotFit) {
  WOLANG_EXPECT(Fixture("fun f() { let x: u8 = 300 }").HasCode("E0109"));
  WOLANG_EXPECT(Fixture("fun f() { let x: i8 = 200 }").HasCode("E0109"));
  // The boundary values fit.
  WOLANG_EXPECT(Fixture("fun f() { let x: u8 = 255 }").Codes().empty());
  WOLANG_EXPECT(Fixture("fun f() { let x: i8 = 127 }").Codes().empty());
}

WOLANG_TEST(TypeCheck, SuffixesFixTheTypeRegardlessOfContext) {
  Fixture fixture(
      "fun f() {\n"
      "  let a = 1u8\n"
      "  let b = 2.0f64\n"
      "  let c = 3i64\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "u8");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "f64");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "i64");
}

WOLANG_TEST(TypeCheck, InfersLiteralKinds) {
  Fixture fixture(
      "fun f() {\n"
      "  let a = true\n"
      "  let b = \"text\"\n"
      "  let c = 'x'\n"
      "  let d = 1.5\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "bool");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "String");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "char");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("d"), "f32");
}

WOLANG_TEST(TypeCheck, ReportsMismatchedTypes) {
  WOLANG_EXPECT(
      Fixture("fun f() { let x: bool = 1 }").HasCode("E0101"));
  WOLANG_EXPECT(
      Fixture("fun f() { let x: int = true }").HasCode("E0101"));
  WOLANG_EXPECT(
      Fixture("fun f() -> int { return true }").HasCode("E0101"));
  // A float literal does not silently become an integer.
  WOLANG_EXPECT(Fixture("fun f() { let x: int = 1.5 }").HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, ChecksOperators) {
  Fixture fixture(
      "fun f(flag: bool, n: int) {\n"
      "  let a = n + 1\n"
      "  let b = n < 2\n"
      "  let c = flag && true\n"
      "  let d = -n\n"
      "  let e = !flag\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "i32");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "bool");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "bool");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("d"), "i32");

  WOLANG_EXPECT(Fixture("fun f(s: str) { let a = s + 1 }").HasCode("E0108"));
  WOLANG_EXPECT(Fixture("fun f(s: str) { let a = -s }").HasCode("E0108"));
  WOLANG_EXPECT(Fixture("fun f(n: int) { let a = n && true }")
                    .HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, LiteralsOnEitherSideTakeTheOthersType) {
  Fixture fixture(
      "fun f(small: u8) {\n"
      "  let a = small + 1\n"
      "  let b = 1 + small\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "u8");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "u8");
}

// -------------------------------------------------------------- scopes -----

WOLANG_TEST(TypeCheck, ReportsAnUnknownName) {
  Fixture fixture("fun f() { let x = nothing_here }");
  WOLANG_EXPECT(fixture.HasCode("E0103"));
  WOLANG_EXPECT(fixture.Messages().find("no value named `nothing_here`") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, ParametersAndLocalsAreInScope) {
  Fixture fixture(
      "fun f(a: int) {\n"
      "  let b = a\n"
      "  let c = b\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "i32");
}

WOLANG_TEST(TypeCheck, ALocalDoesNotEscapeItsBlock) {
  Fixture fixture(
      "fun f() {\n"
      "  if true {\n"
      "    let inner = 1\n"
      "  }\n"
      "  let x = inner\n"
      "}\n");
  WOLANG_EXPECT(fixture.HasCode("E0103"));
}

WOLANG_TEST(TypeCheck, ReportsAssignmentToALetBinding) {
  WOLANG_EXPECT(
      Fixture("fun f() {\n  let a = 1\n  a = 2\n}\n").HasCode("E0105"));
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
      "  let a: Pawn? = p\n"
      "  let b: Pawn? = null\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "Pawn?");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "Pawn?");
}

WOLANG_TEST(TypeCheck, ReportsAFieldAccessThroughAnOptional) {
  Fixture fixture(
      "struct Pawn {}\n"
      "fun f(p: Pawn?) {\n"
      "  let a = p.field\n"
      "}\n");
  WOLANG_EXPECT(fixture.HasCode("E0104"));
  WOLANG_EXPECT(fixture.Messages().find("may be absent") !=
                std::string::npos);
}

WOLANG_TEST(TypeCheck, IfLetNarrowsAnOptional) {
  Fixture fixture(
      "struct Pawn {}\n"
      "fun f(p: Pawn?) {\n"
      "  if let inner = p {\n"
      "    let a = inner\n"
      "  }\n"
      "}\n");
  // The narrowed binding is no longer optional, so using it is fine.
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "Pawn");
}

WOLANG_TEST(TypeCheck, PresentPatternNarrowsAnOptional) {
  Fixture fixture(
      "fun f(count: int?) -> int {\n"
      "  match count {\n"
      "    n? => n\n"
      "    null => 0\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(TypeCheck, NullCoalescingRemovesOneLayer) {
  Fixture fixture(
      "fun f(count: int?) {\n"
      "  let a = count ?? 0\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "i32");

  WOLANG_EXPECT(
      Fixture("fun f(n: int) { let a = n ?? 0 }").HasCode("E0108"));
  WOLANG_EXPECT(
      Fixture("fun f(n: int?) { let a = n ?? true }").HasCode("E0101"));
}

// ---------------------------------------------------------------- units -----

WOLANG_TEST(TypeCheck, UnitLiteralsCarryTheirDimension) {
  Fixture fixture(
      "dimension Length\n"
      "dimension Time\n"
      "unit m: Length\n"
      "unit s: Time\n"
      "fun f() {\n"
      "  let distance = 10m\n"
      "  let duration = 2s\n"
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
      "  let speed = 10m / 2s\n"
      "  let area = 3m * 4m\n"
      "  let ratio = 10m / 2m\n"
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
      "  let bad = 10m + 2s\n"
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
      "fun f() { let bad = 10m < 2s }\n");
  WOLANG_EXPECT(fixture.HasCode("E1001"));
}

WOLANG_TEST(TypeCheck, SameDimensionAddsFine) {
  Fixture fixture(
      "dimension Length\n"
      "unit m: Length\n"
      "unit cm = 0.01m\n"
      "fun f() {\n"
      "  let total = 10m + 50cm\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  // A derived unit shares its base unit's dimension.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("total"), "Length");
}

WOLANG_TEST(TypeCheck, ReportsAnUnknownUnit) {
  Fixture fixture("fun f() { let x = 10parsecs }");
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
      "fun f(a: Vec3, b: Turret?, c: [Vec3], d: [Vec3; 3]) {\n"
      "  let x = a\n"
      "  let y = c\n"
      "  let z = d\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("x"), "Vec3");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("y"), "[Vec3]");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("z"), "[Vec3; 3]");
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
      "  let x = a\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  // The parameter shadows the struct, as the innermost scope.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("x"), "T");
}

WOLANG_TEST(TypeCheck, ArrayLiteralsTakeTheirElementType) {
  Fixture fixture(
      "fun f() {\n"
      "  let a = [1, 2, 3]\n"
      "  let b: [u8; 2] = [1, 2]\n"
      "  let c = [1.5, 2.5]\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "[i32; 3]");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "[u8; 2]");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("c"), "[f32; 2]");

  WOLANG_EXPECT(
      Fixture("fun f() { let a = [1, true] }").HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, IndexingYieldsTheElementType) {
  Fixture fixture(
      "fun f(xs: [float]) {\n"
      "  let a = xs[0]\n"
      "  let b = xs[1..]\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "f32");
  // Indexing with a range produces a slice.
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "[f32]");

  WOLANG_EXPECT(Fixture("fun f(n: int) { let a = n[0] }").HasCode("E0107"));
}

WOLANG_TEST(TypeCheck, ForBindsTheElementType) {
  Fixture fixture(
      "fun f(xs: [float]) {\n"
      "  for item in xs {\n"
      "    let a = item\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "f32");
}

WOLANG_TEST(TypeCheck, TuplesCheckElementwise) {
  Fixture fixture(
      "fun f() {\n"
      "  let a = (1, true)\n"
      "  let b: (u8, bool) = (2, false)\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "(i32, bool)");
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
      "  let a = if flag { 1 } else { 2 }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "i32");

  WOLANG_EXPECT(
      Fixture("fun f(flag: bool) { let a = if flag { 1 } else { true } }")
          .HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, CastsTakeTheTargetType) {
  Fixture fixture(
      "fun f(n: int) {\n"
      "  let a = n as i64\n"
      "  let b = n as f64\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("a"), "i64");
  WOLANG_EXPECT_EQ(fixture.TypeOfInit("b"), "f64");
}

WOLANG_TEST(TypeCheck, ErrorTypesDoNotCascade) {
  // One unknown name produces exactly one message, not one per use.
  Fixture fixture(
      "fun f() {\n"
      "  let a = unknown\n"
      "  let b = a + 1\n"
      "  let c = b * 2\n"
      "  let d: bool = c\n"
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
  let a = v.x
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
  let a = v.z
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
    let left = self.health
  }
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("left"), "f32");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AssigningAnImmutableFieldIsReported) {
  Fixture f(R"(
class Pawn {
  let health: float = 100.0

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
  let bad = v.x
  let good = v?.x
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
  let a = Damage(1.0, 2.0)
  let b = Damage(1.0)
  let c = Damage(1.0, true)
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
  let a = Vec3(1.0, 2.0)
  let b = Vec3()
  let c = Vec3(1.0)
  let d = Vec3(1.0, true)
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
  let eta = Travel(3m, 2m / 1s)
  let bad = Travel(3s, 2m / 1s)
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


// ------------------------------------------------- generics and traits -----

WOLANG_TEST(TypeCheck, GenericCallsInferTheirParameters) {
  Fixture f(R"(
fun First<T>(items: [T]) -> T {
  return items[0]
}

fun use(xs: [int], ys: [float]) {
  let a = First(xs)
  let b = First(ys)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("a"), "i32");
  WOLANG_EXPECT_EQ(f.TypeOfInit("b"), "f32");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, InferenceAcrossSeveralParameters) {
  Fixture f(R"(
fun Pair<A, B>(a: A, b: B) -> (A, B) {
  return (a, b)
}

fun use() {
  let p = Pair(1, true)
}
)");
  WOLANG_EXPECT_EQ(f.TypeOfInit("p"), "(i32, bool)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
}

WOLANG_TEST(TypeCheck, AnArgumentThatCannotMatchIsReported) {
  Fixture f(R"(
fun First<T>(items: [T]) -> T {
  return items[0]
}

fun use() {
  let a = First(1)
}
)");
  WOLANG_EXPECT(f.HasCode("E0101"));
}

WOLANG_TEST(TypeCheck, InferredResultStillMeetsItsExpectation) {
  Fixture f(R"(
fun First<T>(items: [T]) -> T {
  return items[0]
}

fun use(xs: [int]) {
  let a: bool = First(xs)
}
)");
  WOLANG_EXPECT(f.HasCode("E0101"));
  WOLANG_EXPECT(f.Messages().find("expected `bool`, found `i32`") !=
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

WOLANG_TEST(TypeCheck, MissingTraitMethodIsReported) {
  Fixture f(R"(
trait Damageable {
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

WOLANG_TEST(TypeCheck, ATraitSatisfiedByAnotherExtensionIsAccepted) {
  // The methods may come from anywhere, not only from the block that names
  // the trait.
  Fixture f(R"(
trait Damageable {
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

}  // namespace
}  // namespace wolang
