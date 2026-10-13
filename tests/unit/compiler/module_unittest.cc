// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/module.h"

#include <string>
#include <vector>

#include "harness/test.h"
#include "wolang/compiler/database.h"

namespace wolang {
namespace {

// Builds a workspace in memory: no filesystem, so the module graph under test
// is exactly the one the test wrote down.
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

  std::string Codes(FileId file) {
    std::string result;
    const std::vector<Diagnostic>* diagnostics = db_.Diagnostics(file);
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
  fixture.AddFile(module, "a.wo", "pub fn Fire() {}\nclass Turret {}\n");
  fixture.AddFile(module, "b.wo", "pub struct Hit {}\n");

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
                  "import other.place\n"
                  "class C {\n  var field: int = 0\n  fn Method() {}\n}\n"
                  "impl C { fn Extra() {} }\n");

  const ModuleScope* scope = fixture.db().Scope(module);
  // Only `C` is a module-level name.
  WOLANG_ASSERT_EQ(scope->entries.size(), size_t{1});
  WOLANG_EXPECT_EQ(scope->entries.front().kind, ItemKind::kClass);
}

WOLANG_TEST(ModuleScopeTest, ReportsDuplicateDeclarationsAcrossFiles) {
  Fixture fixture;
  const ModuleId module = fixture.AddModule("game");
  const FileId first = fixture.AddFile(module, "a.wo", "fn Fire() {}\n");
  const FileId second = fixture.AddFile(module, "b.wo", "fn Fire() {}\n");

  const ModuleScope* scope = fixture.db().Scope(module);
  WOLANG_ASSERT_EQ(scope->duplicates.size(), size_t{1});

  // Both declarations are blamed, each in its own file.
  WOLANG_EXPECT_EQ(fixture.Codes(first), "E0057");
  WOLANG_EXPECT_EQ(fixture.Codes(second), "E0057");
  WOLANG_EXPECT(fixture.Messages(first).find("declared more than once") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ResolvesImportsBetweenModules) {
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "pub struct Vec3 {}\npub fn Dot() {}\n");

  const ModuleId game = fixture.AddModule("game");
  const FileId main = fixture.AddFile(
      game, "main.wo", "import game.math.{Vec3, Dot}\nfn main() {}\n");

  const ResolvedImports* imports = fixture.db().Imports(main);
  WOLANG_ASSERT_NE(imports, nullptr);
  WOLANG_EXPECT(!imports->outside_workspace);
  WOLANG_ASSERT_EQ(imports->imports.size(), size_t{1});
  WOLANG_EXPECT_EQ(imports->imports.front().module, math);
  WOLANG_EXPECT_EQ(imports->imports.front().names.size(), size_t{2});
  WOLANG_EXPECT(imports->imports.front().missing.empty());
  WOLANG_EXPECT(fixture.Codes(main).empty());
}

WOLANG_TEST(ModuleScopeTest, ReportsAnUnknownModule) {
  Fixture fixture;
  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "import game.missing\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0055");
  WOLANG_EXPECT(fixture.Messages(main).find("no module named `game.missing`") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, ReportsAnUnknownImportedName) {
  Fixture fixture;
  const ModuleId math = fixture.AddModule("game.math");
  fixture.AddFile(math, "vec.wo", "pub struct Vec3 {}\n");
  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "import game.math.{Vec3, Vec4}\n");

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
      fixture.AddFile(game, "main.wo", "import game.math.{Hidden}\n");

  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0058");
  WOLANG_EXPECT(fixture.Messages(main).find("`Hidden` is not public") !=
                std::string::npos);
}

WOLANG_TEST(ModuleScopeTest, AFileOutsideAnyModuleResolvesNothing) {
  Database db;
  const FileId file = db.AddFile("loose.wo", "import whatever.place\n");

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
      fixture.AddFile(module, "a.wo", "pub fn Fire() { Shoot() }\n");
  fixture.db().Scope(module);
  fixture.db().engine().ResetCounters();

  fixture.db().SetFileText(file, "pub fn Fire() { Shoot()\n  Reload() }\n");
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
  const FileId vec = fixture.AddFile(math, "vec.wo", "pub fn Dot() {}\n");
  const ModuleId game = fixture.AddModule("game");
  const FileId main =
      fixture.AddFile(game, "main.wo", "import game.math.{Dot}\n");
  WOLANG_EXPECT(fixture.Codes(main).empty());

  fixture.db().SetFileText(vec, "pub fn Cross() {}\n");
  WOLANG_EXPECT_EQ(fixture.Codes(main), "E0056");
}

WOLANG_TEST(ModuleScopeTest, ApplyingTheManifestLintPolicy) {
  Database db;
  Manifest manifest;
  manifest.name = "game";
  manifest.lints.push_back(LintOverride{"E0024", Severity::kOff});
  const PackageId package = db.AddPackage("game", "/game", manifest);

  const ModuleId module = db.AddModule("game", "/game/src", package);
  const FileId file = db.AddFile("a.wo", "fn f() { let a = * }\n");
  db.SetModuleFiles(module, {file});

  // The syntax error is real, but this package turned that code off.
  WOLANG_EXPECT(db.Diagnostics(file)->empty());
}

WOLANG_TEST(ModuleScopeTest, ImportPathTextJoinsSegments) {
  Database db;
  const FileId file = db.AddFile("a.wo", "import engine.math.vec\n");
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
