// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/compiler/module.h"

#include <string>
#include <vector>

#include "harness/test.h"
#include "wolang/compiler/database.h"
#include "wolang/ir/printer.h"

namespace wolang {
namespace {

// Builds a workspace in memory: no filesystem, so the module graph under test
// is exactly the one the test wrote down.
// A lowering problem list as one string, so a failure shows what it said.
std::string JoinProblems(const std::vector<std::string>& problems) {
  std::string result;
  for (const std::string& problem : problems) {
    if (!result.empty()) {
      result.push_back('\n');
    }
    result.append(problem);
  }
  return result;
}

class Fixture {
 public:
  ModuleId AddModule(std::string name) {
    return db_.AddModule(std::move(name), "", PackageId{});
  }

  FileId AddFile(ModuleId module,
                 std::string path,
                 std::string text) {
    const FileId file = db_.AddFile(std::move(path), std::move(text));
    const Module* existing = db_.GetModule(module);
    std::vector<FileId> files =
        existing != nullptr ? existing->files : std::vector<FileId>();
    files.push_back(file);
    db_.SetModuleFiles(module, std::move(files));
    return file;
  }

  Database& db() { return db_; }

  // Errors only. A warning is asserted through Warnings(), so that adding a
  // lint does not invalidate every test that says "this resolves".
  std::string Codes(FileId file) {
    return CodesOf(file, Severity::kError);
  }
  std::string Warnings(FileId file) {
    return CodesOf(file, Severity::kWarning);
  }

  std::string CodesOf(FileId file, Severity severity) {
    std::string result;
    const std::vector<Diagnostic>* diagnostics = db_.Diagnostics(file);
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

  std::string Messages(FileId file) {
    std::string result;
    const std::vector<Diagnostic>* diagnostics = db_.Diagnostics(file);
    if (diagnostics == nullptr) {
      return result;
    }
    for (const Diagnostic& diagnostic : *diagnostics) {
      result.append(diagnostic.message);
      result.push_back('\n');
    }
    return result;
  }

 private:
  Database db_;
};

WOLANG_TEST(ModuleScopeTest, CollectsDeclarationsFromEveryFile) {
  Fixture fixture;
  const ModuleId module = fixture.AddModule("game");
  fixture.AddFile(module, "a.wo", "offer fun Fire() {}\nclass Turret {}\n");
  fixture.AddFile(module, "b.wo", "offer struct Hit {}\n");

  const ModuleScope* scope = fixture.db().Scope(module);
  WOLANG_ASSERT_NE(scope, nullptr);
  WOLANG_EXPECT_EQ(scope->entries.size(), size_t{3});
  WOLANG_EXPECT(scope->duplicates.empty());

  const Symbol fire = fixture.db().interner().Lookup("Fire");
  const ScopeEntry* entry = scope->Find(fire);
  WOLANG_ASSERT_NE(entry, nullptr);
  WOLANG_EXPECT_EQ(entry->kind, ItemKind::kFunction);
  WOLANG_EXPECT(entry->is_public);

  const ScopeEntry* turret =
      scope->Find(fixture.db().interner().Lookup("Turret"));
  WOLANG_ASSERT_NE(turret, nullptr);
  WOLANG_EXPECT(!turret->is_public);
}

WOLANG_TEST(ModuleScopeTest, IgnoresMembersImportsAndImpls) {
  Fixture fixture;
  const ModuleId module = fixture.AddModule("game");
  fixture.AddFile(module, "a.wo",
                  "need other.place\n"
                  "class C {\n  var field: int = 0\n  fun Method() {}\n}\n"
                  "impl C { fun Extra() {} }\n");

  const ModuleScope* scope = fixture.db().Scope(module);
  // Only `C` is a module-level name.
  WOLANG_ASSERT_EQ(scope->entries.size(), size_t{1});
  WOLANG_EXPECT_EQ(scope->entries.front().kind, ItemKind::kClass);
}

WOLANG_TEST(ModuleScopeTest, ReportsDuplicateDeclarationsAcrossFiles) {
  Fixture fixture;
  const ModuleId module = fixture.AddModule("game");
  const FileId first = fixture.AddFile(module, "a.wo", "fun Fire() {}\n");
  const FileId second = fixture.AddFile(module, "b.wo", "fun Fire() {}\n");

  const ModuleScope* scope = fixture.db().Scope(module);
  WOLANG_ASSERT_EQ(scope->duplicates.size(), size_t{1});

  // Both declarations are blamed, each in its own file.
  WOLANG_EXPECT_EQ(fixture.Codes(first), "E0057");
  WOLANG_EXPECT_EQ(fixture.Codes(second), "E0057");
  WOLANG_EXPECT(fixture.Messages(first).find("declared more than once") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, InstantiatesAGenericFromAnotherModule) {
  // A generic is a recipe, and the module it was written in cannot know
  // what its importers will ask for -- so the copy is made where it is
  // used, with the defining file's names still meaning what they meant
  // there. Without this, a standard library of generic containers is a
  // library nothing outside it can call.
  Fixture fixture;
  const ModuleId library = fixture.AddModule("box");
  fixture.AddFile(library, "box.wo", R"(
offer struct Holder<T> {
  var item: T

  fun Item() -> T {
    return item
  }
}

offer fun Wrap<T>(value: T) -> Holder<T> {
  return Holder<T>(value)
}
)");
  const ModuleId app = fixture.AddModule("app");
  const FileId main = fixture.AddFile(app, "main.wo", R"(
need box.{Wrap}

fun Use() -> int {
  const held = Wrap<int>(7)
  return held.Item()
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(main), "");

  const LoweredFile* lowered = fixture.db().Lowered(main);
  WOLANG_ASSERT_NE(lowered, nullptr);
  WOLANG_EXPECT_EQ(JoinProblems(lowered->problems), "");
  const std::string text =
      ir::PrintModule(fixture.db().interner(), fixture.db().types(),
                      lowered->module);
  // The copy is here, with a body, and so is the method of the instance
  // it returns.
  WOLANG_EXPECT(text.find("fun @Wrap<s64>") != std::string::npos);
  WOLANG_EXPECT(text.find("fun @Holder<s64>.Item") != std::string::npos);
  WOLANG_EXPECT(text.find("fun @Wrap<s64>(s64) -> Holder<s64>\n") ==
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AQualifiedNameReachesItsModulesItem) {
  // `a.Parse` and `b.Parse` are two functions, and each is reached by its
  // module's name (D246). Before, `a` bound to nothing and the call lowered
  // to an undefined value without a word.
  Fixture fixture;
  const ModuleId a = fixture.AddModule("a");
  fixture.AddFile(a, "a.wo", "offer fun Parse() -> int { return 1 }\n");
  const ModuleId b = fixture.AddModule("b");
  fixture.AddFile(b, "b.wo", "offer fun Parse() -> int { return 2 }\n");
  const ModuleId app = fixture.AddModule("app");
  const FileId main = fixture.AddFile(app, "main.wo", R"(
need a
need b

fun Use() -> int {
  return a.Parse() * 10 + b.Parse()
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(main), "");
  const LoweredFile* lowered = fixture.db().Lowered(main);
  WOLANG_ASSERT_NE(lowered, nullptr);
  WOLANG_EXPECT_EQ(JoinProblems(lowered->problems), "");
  const std::string text =
      ir::PrintModule(fixture.db().interner(), fixture.db().types(),
                      lowered->module);
  WOLANG_EXPECT(text.find("@a.Parse") != std::string::npos);
  WOLANG_EXPECT(text.find("@b.Parse") != std::string::npos);
  WOLANG_EXPECT(text.find("undef") == std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AnUnknownQualifiedNameIsReported) {
  Fixture fixture;
  const ModuleId a = fixture.AddModule("a");
  fixture.AddFile(a, "a.wo",
                  "offer fun Parse() -> int { return 1 }\n"
                  "fun Hidden() -> int { return 2 }\n");
  const ModuleId app = fixture.AddModule("app");
  const FileId missing = fixture.AddFile(app, "m.wo", R"(
need a

fun Use() -> int {
  return a.Nothing()
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(missing), "E0144");
  const ModuleId app2 = fixture.AddModule("app2");
  const FileId hidden = fixture.AddFile(app2, "h.wo", R"(
need a

fun Use2() -> int {
  return a.Hidden()
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(hidden), "E0144");
}

WOLANG_TEST(ModuleScopeTest, AValueNamedLikeAModuleStillMeansTheValue) {
  // The non-firing twin: a parameter called `a` is the value, not the module.
  Fixture fixture;
  const ModuleId a = fixture.AddModule("a");
  fixture.AddFile(a, "a.wo", "offer fun Parse() -> int { return 1 }\n");
  const ModuleId app = fixture.AddModule("app");
  const FileId main = fixture.AddFile(app, "main.wo", R"(
need a

struct Pair {
  var Nothing: int = 0
}

fun Use(a: Pair) -> int {
  return a.Nothing
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(main), "");
}

WOLANG_TEST(ModuleScopeTest, TwoModulesOfferingOneNameMakeTheBareNameAmbiguous) {
  Fixture fixture;
  const ModuleId a = fixture.AddModule("a");
  fixture.AddFile(a, "a.wo", "offer fun Parse() -> int { return 1 }\n");
  const ModuleId b = fixture.AddModule("b");
  fixture.AddFile(b, "b.wo", "offer fun Parse() -> int { return 2 }\n");
  const ModuleId app = fixture.AddModule("app");
  const FileId bare = fixture.AddFile(app, "bare.wo", R"(
need a
need b

fun Use() -> int {
  return Parse()
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(bare), "E0143");

  // Not ambiguous when the two are told apart by name.
  const ModuleId app2 = fixture.AddModule("app2");
  const FileId told = fixture.AddFile(app2, "told.wo", R"(
need a
need b

fun Use() -> int {
  return a.Parse() + b.Parse()
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(told), "");
}

WOLANG_TEST(ModuleScopeTest, AMethodOnATypeTheCallerNeverImported) {
  // `Rows()` hands back a `Holder<int>`, and the caller never mentions
  // `Holder`. It should still be able to call a method on what it was
  // given -- the type came with the value.
  //
  // Member tables were merged from the direct imports only, so the
  // caller had a type it knew the name of and no members: the call
  // quietly became an error value and the program trapped at run time
  // with nothing explaining why (D190).
  Fixture fixture;
  const ModuleId library = fixture.AddModule("box");
  fixture.AddFile(library, "box.wo", R"(
offer struct Holder<T> {
  var item: T

  fun Item() -> T {
    return item
  }
}
)");

  const ModuleId middle = fixture.AddModule("middle");
  fixture.AddFile(middle, "middle.wo", R"(
need box.{Holder}

offer fun Rows() -> Holder<int> {
  return Holder<int>(7)
}
)");

  const ModuleId app = fixture.AddModule("app");
  const FileId main = fixture.AddFile(app, "main.wo", R"(
need middle.{Rows}

fun Use() -> int {
  const held = Rows()
  return held.Item()
}
)");
  WOLANG_EXPECT_EQ(fixture.Codes(main), "");

  const LoweredFile* lowered = fixture.db().Lowered(main);
  WOLANG_ASSERT_NE(lowered, nullptr);
  WOLANG_EXPECT_EQ(JoinProblems(lowered->problems), "");
  const std::string text =
      ir::PrintModule(fixture.db().interner(), fixture.db().types(),
                      lowered->module);
  // The method is a call, not an undefined value.
  WOLANG_EXPECT(text.find("undef : <error>") == std::string::npos);
  WOLANG_EXPECT(text.find("Holder<s64>.Item") != std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ResolvesImportsBetweenModules) {
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "offer struct Vec3 {}\noffer fun Dot() {}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo", "need game.math.{Vec3, Dot}\nfun main() {}\n");

  const ResolvedImports* imports = fixture.db().Imports(main);
  WOLANG_ASSERT_NE(imports, nullptr);
  WOLANG_EXPECT(!imports->outside_workspace);
  WOLANG_ASSERT_EQ(imports->imports.size(), size_t{1});
  WOLANG_EXPECT_EQ(imports->imports.front().module, math);
  WOLANG_EXPECT_EQ(imports->imports.front().names.size(), size_t{2});
  WOLANG_EXPECT(imports->imports.front().missing.empty());
  WOLANG_EXPECT(fixture.Codes(main).empty());
}

WOLANG_TEST(ModuleScopeTest, ReportsANeedNothingUses) {
  // Only type checking knows what a file actually named, which is why this
  // lives with the checker rather than with import resolution.
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "offer struct Vec3 {}\noffer fun Dot() {}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "need game.math.{Vec3, Dot}\nfun main() { Dot() }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "");
  WOLANG_EXPECT_EQ(fixture.Warnings(main), "W0101");
  WOLANG_EXPECT(fixture.Messages(main).find("`Vec3` is needed but never used") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ANameUsedInTypePositionCounts) {
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "offer struct Vec3 {}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "need game.math.{Vec3}\nfun take(_v: Vec3) {}\n");

  WOLANG_EXPECT_EQ(fixture.Warnings(main), "");
}

WOLANG_TEST(ModuleScopeTest, AnAliasFromAnotherModuleIsItsTarget) {
  Fixture fixture;
  const ModuleId assets = fixture.AddModule("game.assets");
  fixture.AddFile(assets, "mesh.wo", "offer type MeshAsset = text\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId fits = fixture.AddFile(game, "fits.wo", "need game.assets.{MeshAsset}\nfun Name(m: MeshAsset) -> text { return m }\n");
  WOLANG_EXPECT_EQ(fixture.Codes(fits), "");

  const FileId clashes = fixture.AddFile(game, "clashes.wo", "need game.assets.{MeshAsset}\nfun Count(m: MeshAsset) -> int { return m }\n");
  WOLANG_EXPECT(!fixture.Codes(clashes).empty());
}

WOLANG_TEST(ModuleScopeTest, ExtendingAForeignClassSeesItsFields) {
  Fixture fixture;
  const ModuleId engine = fixture.AddModule("game.engine");
  fixture.AddFile(engine, "pawn.wo", "offer class Pawn {\n  var health: int\n}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId reads = fixture.AddFile(game, "heal.wo", "need game.engine.{Pawn}\nextend Pawn {\n  fun Health() -> int { return self.health }\n}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(reads), "");

  const FileId misses = fixture.AddFile(game, "miss.wo", "need game.engine.{Pawn}\nextend Pawn {\n  fun Mana() -> int { return self.mana }\n}\n");
  WOLANG_EXPECT(!fixture.Codes(misses).empty());
}

WOLANG_TEST(ModuleScopeTest, ABaseClassCountsAsUsed) {
  Fixture fixture;
  const ModuleId engine = fixture.AddModule("game.engine");
  fixture.AddFile(engine, "actor.wo", "offer class Actor {}\noffer fun Spare() {}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(game, "main.wo", "need game.engine.{Actor}\nclass Pawn : Actor {}\n");
  WOLANG_EXPECT_EQ(fixture.Warnings(main), "");

  const FileId unused = fixture.AddFile(game, "other.wo", "need game.engine.{Actor, Spare}\nclass Pawn2 : Actor {}\n");
  WOLANG_EXPECT_EQ(fixture.Warnings(unused), "W0101");
}

WOLANG_TEST(ModuleScopeTest, ABareNeedIsNeverUnused) {
  // `need a.b` brings in a whole module, so there is no one name to blame.
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "offer struct Vec3 {}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "need game.math\nfun main() {}\n");

  WOLANG_EXPECT_EQ(fixture.Warnings(main), "");
}

// ------------------------------------------------------------- effects -----

WOLANG_TEST(ModuleScopeTest, ReportsAnEffectADeclarationDoesNotState) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "effect Telemetry\n"
      "fun Report() uses io { Measure() }\n"
      "fun Measure() uses Telemetry {}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0701");
  WOLANG_EXPECT(fixture.Messages(main).find("does `Telemetry`") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AStatedRowThatCoversTheBodyIsAccepted) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "effect Telemetry\n"
      "fun Report() uses io, Telemetry { Measure() }\n"
      "fun Measure() uses Telemetry {}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "");
}

WOLANG_TEST(ModuleScopeTest, ADeclarationThatStatesNothingIsNotContradicted) {
  // Only a stated row can be contradicted; the body's effects are still
  // inferred, for callers to read.
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "fun Quiet() { Loud() }\n"
      "fun Loud() uses io {}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "");
}

WOLANG_TEST(ModuleScopeTest, EffectsTravelThroughAFunctionWithNoRow) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "effect Telemetry\n"
      "fun Top() uses io { Middle() }\n"
      "fun Middle() { Measure() }\n"
      "fun Measure() uses Telemetry {}\n");

  // Two hops, the middle one stating nothing of its own.
  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0701");
}

WOLANG_TEST(ModuleScopeTest, EffectsPropagateThroughMutualRecursion) {
  // The reason the fixpoint is one query per module rather than one per
  // declaration: a cycle here must not make the answer depend on which
  // declaration was asked about first.
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "effect Telemetry\n"
      "fun Ping() uses io { Pong() }\n"
      "fun Pong() { Ping()\n  Emit() }\n"
      "fun Emit() uses Telemetry {}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0701");
  WOLANG_EXPECT(fixture.Messages(main).find("`Ping` does `Telemetry`") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AnExposesFunctionPassesItsUnsafeToTheCaller) {
  // `Bytes` hands out a writable view of text that may be a literal; the
  // caller is the one who answers for writing through it (D233).
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId exposed = fixture.AddFile(
      game, "a.wo",
      "@Exposes\n"
      "fun View() uses unsafe { }\n"
      "fun Quiet() uses io { View() }\n");
  WOLANG_EXPECT_EQ(fixture.Codes(exposed), "E0701");

  Fixture absorbed_fixture;
  const ModuleId other = absorbed_fixture.AddModule("game");
  const FileId absorbed = absorbed_fixture.AddFile(
      other, "a.wo",
      "fun View() uses unsafe { }\n"
      "fun Quiet() uses io { View() }\n");
  WOLANG_EXPECT_EQ(absorbed_fixture.Codes(absorbed), "");
}

WOLANG_TEST(ModuleScopeTest, AnUnsafeBlockIsTheUnsafeEffect) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo", "fun Poke() uses io { unsafe { } }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0701");
  WOLANG_EXPECT(fixture.Messages(main).find("does `unsafe`") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AnOfferedFunctionMustStateItsEffects) {
  // Inference stops at the module boundary, so a name other modules can call
  // has to say what it does.
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "offer fun Emit() { Inner() }\n"
      "fun Inner() uses io {}\n"
      "offer fun Pure() -> int => 1\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0704");
  WOLANG_EXPECT(fixture.Messages(main).find("`Emit` is offered") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AnImportedRowIsReadFromItsSignature) {
  Fixture fixture;
  const ModuleId sub = fixture.AddModule("game.sub");
  fixture.AddFile(sub, "emit.wo", "offer fun Emit() uses io {}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "need game.sub.{Emit}\nfun Call() uses rand { Emit() }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0701");
  WOLANG_EXPECT(fixture.Messages(main).find("does `io`") != std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ADeterministicFunctionMayNotReachRandOrTime) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "@Deterministic\n"
      "fun Simulate() uses rand { Roll() }\n"
      "fun Roll() uses rand {}\n");

  // The context outranks the signature: declaring `rand` does not make it
  // allowed here.
  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0702");
  WOLANG_EXPECT(fixture.Messages(main).find("not allowed here") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ADeterministicFunctionWithNoEffectsIsFine) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo", "@Deterministic\nfun Pure() -> int => 1\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "");
}

WOLANG_TEST(ModuleScopeTest, AHotFunctionMayNotAllocate) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "class Box {}\n@Hot\nfun Tick() { Grow() }\nfun Grow() { const b = Box() }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0155");
}

WOLANG_TEST(ModuleScopeTest, AHotFunctionThatDoesNotAllocateIsFine) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "struct P { var x: int }\n@Hot\nfun Tick() -> int { const p = P(1)\n return p.x + 1 }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "");
}

WOLANG_TEST(ModuleScopeTest, AHotPathReachesAnotherModulesMethod) {
  Fixture fixture;
  const ModuleId col = fixture.AddModule("col");
  fixture.AddFile(
      col, "col.wo",
      "offer class Node {}\n"
      "offer class Bag {\n"
      "  fun Push() { const n = Node() }\n"
      "  fun Len() -> int => 0\n"
      "}\n");
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "need col.{Bag}\n"
      "@Hot\nfun Tick(b: Bag) { b.Push() }\n");
  const ModuleId calm = fixture.AddModule("calm");
  const FileId quiet = fixture.AddFile(
      calm, "main.wo",
      "need col.{Bag}\n"
      "@Hot\nfun Tick(b: Bag) -> int { return b.Len() }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0155");
  WOLANG_EXPECT_EQ(fixture.Codes(quiet), "");
}

WOLANG_TEST(ModuleScopeTest, AHotPathMayNotFormatText) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "@Hot\nfun Tick(n: int) -> text { return \"{n}\" }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0155");
}

WOLANG_TEST(ModuleScopeTest, AHotPathMayNotTakeStorage) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "unsafe fun ArrayNew<T>(count: int) -> []T\n"
      "@Hot\nfun Tick() uses unsafe { const a = ArrayNew<int>(4) }\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0155");
}

WOLANG_TEST(ModuleScopeTest, AComptimeFunctionHasNoEffects) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo",
      "comptime fun Validate() { Now() }\nfun Now() uses time {}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0702");
}

WOLANG_TEST(ModuleScopeTest, ReportsAnUnknownModule) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "need game.missing\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0055");
  WOLANG_EXPECT(fixture.Messages(main).find("no module named `game.missing`") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ReportsAnUnknownImportedName) {
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "offer struct Vec3 {}\n");
  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "need game.math.{Vec3, Vec4}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0056");
  WOLANG_EXPECT(fixture.Messages(main).find("`Vec4` is not declared") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ReportsImportingAPrivateName) {
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "struct Hidden {}\n");
  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "need game.math.{Hidden}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0058");
  WOLANG_EXPECT(fixture.Messages(main).find("`Hidden` is not public") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AFileOutsideAnyModuleResolvesNothing) {
  Database db;
  const FileId file = db.AddFile("loose.wo", "need whatever.place\n");

  const ResolvedImports* imports = db.Imports(file);
  WOLANG_ASSERT_NE(imports, nullptr);
  WOLANG_EXPECT(imports->outside_workspace);
  // Checking a single file outside a package must not invent import errors.
  WOLANG_EXPECT(db.Diagnostics(file)->empty());
}

WOLANG_TEST(ModuleScopeTest, EditingABodyKeepsTheModuleScopeGreen) {
  Fixture fixture;
  const ModuleId module = fixture.AddModule("game");
  const FileId file =
      fixture.AddFile(module, "a.wo", "offer fun Fire() { Shoot() }\n");
  fixture.db().Scope(module);
  fixture.db().engine().ResetCounters();

  fixture.db().SetFileText(file, "offer fun Fire() { Shoot()\n  Reload() }\n");
  fixture.db().Scope(module);

  // The file was reparsed and its item tree rebuilt, but the module scope was
  // only verified, not recomputed.
  WOLANG_EXPECT_EQ(
      fixture.db().engine().ComputationCount(QueryKind::kItemTree),
      uint64_t{1});
  WOLANG_EXPECT_EQ(
      fixture.db().engine().ComputationCount(QueryKind::kModuleScope),
      uint64_t{0});
}

WOLANG_TEST(ModuleScopeTest, RenamingADeclarationRecomputesImporters) {
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  const FileId vec = fixture.AddFile(math, "vec.wo", "offer fun Dot() {}\n");
  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "need game.math.{Dot}\n");
  WOLANG_EXPECT(fixture.Codes(main).empty());

  fixture.db().SetFileText(vec, "offer fun Cross() {}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0056");
}

WOLANG_TEST(ModuleScopeTest, ApplyingTheManifestLintPolicy) {
  Database db;
  Manifest manifest;
  manifest.name = "game";
  manifest.lints.push_back(LintOverride{"E0024", Severity::kOff});
  const PackageId package = db.AddPackage("game", "/game", manifest);

  const ModuleId module = db.AddModule("game", "/game/src", package);
  const FileId file = db.AddFile("a.wo", "fun f() { const a = * }\n");
  db.SetModuleFiles(module, {file});

  // The syntax error is real, but this package turned that code off.
  WOLANG_EXPECT(db.Diagnostics(file)->empty());
}

WOLANG_TEST(ModuleScopeTest, ImportPathTextJoinsSegments) {
  Database db;
  const FileId file = db.AddFile("a.wo", "need engine.math.vec\n");
  const ParsedFile* parsed = db.Parse(file);
  WOLANG_ASSERT_NE(parsed, nullptr);

  const std::span<const ItemId> items = parsed->ast.Get(parsed->ast.root());
  WOLANG_ASSERT_EQ(items.size(), size_t{1});
  WOLANG_EXPECT_EQ(
      ImportPathText(db.interner(), parsed->ast, parsed->ast.Get(items[0])),
      "engine.math.vec");
}

}  // namespace
}  // namespace wolang
