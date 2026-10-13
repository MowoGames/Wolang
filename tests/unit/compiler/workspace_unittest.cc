// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/workspace.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "harness/test.h"

namespace wolang {
namespace {

namespace fs = std::filesystem;

int ProcessId() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

// A directory tree created for one test and removed with it. The process id is
// part of the name so that suites running in parallel cannot collide.
class TempTree {
 public:
  explicit TempTree(std::string_view name) {
    std::error_code error;
    root_ = fs::temp_directory_path(error) /
            ("wolang-test-" + std::to_string(ProcessId()) + "-" +
             std::string(name));
    fs::remove_all(root_, error);
    fs::create_directories(root_, error);
  }

  ~TempTree() {
    std::error_code error;
    fs::remove_all(root_, error);
  }

  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  void Write(std::string_view relative, std::string_view text) {
    const fs::path path = root_ / relative;
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  std::string path() const { return root_.string(); }
  std::string path(std::string_view relative) const {
    return (root_ / relative).string();
  }

 private:
  fs::path root_;
};

std::string Codes(const DiagnosticEngine& diagnostics) {
  std::string result;
  for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
    if (!result.empty()) {
      result.push_back(',');
    }
    result.append(diagnostic.code);
  }
  return result;
}

WOLANG_TEST(Workspace, DiscoversModulesFromDirectories) {
  TempTree tree("discover");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\nversion = \"0.1\"\n");
  tree.Write("src/main.wo", "fun main() {}\n");
  tree.Write("src/engine/math.wo", "offer struct Vec3 {}\n");
  tree.Write("src/engine/vec.wo", "offer fun Dot() {}\n");
  tree.Write("src/engine/physics/body.wo", "offer class Body {}\n");
  // Not a source file, and a dot directory: both ignored.
  tree.Write("src/notes.txt", "ignore me\n");
  tree.Write("src/.cache/stale.wo", "fun stale() {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT(Codes(diagnostics).empty());

  WOLANG_EXPECT_EQ(db.package_count(), size_t{1});
  WOLANG_EXPECT_EQ(db.modules().size(), size_t{3});

  const ModuleId root = db.FindModule("game");
  const ModuleId engine = db.FindModule("game.engine");
  const ModuleId physics = db.FindModule("game.engine.physics");
  WOLANG_ASSERT(root.is_valid());
  WOLANG_ASSERT(engine.is_valid());
  WOLANG_ASSERT(physics.is_valid());
  WOLANG_EXPECT(!db.FindModule("game.engine.nothing").is_valid());

  // Two files in one directory form one module.
  WOLANG_EXPECT_EQ(db.GetModule(engine)->files.size(), size_t{2});
  WOLANG_EXPECT_EQ(db.GetModule(root)->files.size(), size_t{1});

  const ModuleScope* scope = db.Scope(engine);
  WOLANG_ASSERT_NE(scope, nullptr);
  WOLANG_EXPECT_EQ(scope->entries.size(), size_t{2});
}

WOLANG_TEST(Workspace, ResolvesImportsAcrossDiscoveredModules) {
  TempTree tree("imports");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\n");
  // The import is used, so that "nothing at all is reported" stays the
  // assertion rather than becoming "nothing but the unused-import lint".
  tree.Write("src/main.wo",
             "need game.engine.{Vec3}\n"
             "fun main() { let _v = Vec3() }\n");
  tree.Write("src/engine/math.wo", "offer struct Vec3 {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT(db.AllDiagnostics().empty());
}

WOLANG_TEST(Workspace, ReportsAnUnresolvedImportInsideAPackage) {
  TempTree tree("unresolved");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\n");
  tree.Write("src/main.wo", "need game.nowhere\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));

  const std::vector<Diagnostic> all = db.AllDiagnostics();
  WOLANG_ASSERT_EQ(all.size(), size_t{1});
  WOLANG_EXPECT_EQ(all.front().code, "E0055");
}

WOLANG_TEST(Workspace, LoadsPathDependencies) {
  TempTree tree("deps");
  tree.Write("game/wolang.toml",
             "[package]\nname = \"game\"\n"
             "[dependencies]\ncore = { path = \"../core\" }\n");
  tree.Write("game/src/main.wo", "need core.{Log}\nfun main() { Log() }\n");
  tree.Write("core/wolang.toml", "[package]\nname = \"core\"\n");
  tree.Write("core/src/log.wo", "offer fun Log() {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path("game"), &diagnostics));
  WOLANG_EXPECT(Codes(diagnostics).empty());

  WOLANG_EXPECT_EQ(db.package_count(), size_t{2});
  WOLANG_EXPECT(db.FindModule("core").is_valid());
  WOLANG_EXPECT(db.AllDiagnostics().empty());
}

WOLANG_TEST(Workspace, ADenyingManifestForbidsUnsafe) {
  // `unsafe` is an effect, so a package-wide ban is one more row a module
  // may not have -- the same mechanism, not a separate check.
  TempTree tree("nounsafe");
  tree.Write("wolang.toml",
             "[package]\nname = \"game\"\n[build]\nunsafe = \"deny\"\n");
  tree.Write("src/main.wo",
             "fun Poke() uses unsafe { unsafe { } }\n"
             "fun Fine() -> int => 1\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));

  const std::vector<Diagnostic> all = db.AllDiagnostics();
  WOLANG_ASSERT_EQ(all.size(), size_t{1});
  WOLANG_EXPECT_EQ(all.front().code, "E0801");
}

WOLANG_TEST(Workspace, AnAllowingManifestPermitsUnsafe) {
  TempTree tree("withunsafe");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\n");
  tree.Write("src/main.wo", "fun Poke() uses unsafe { unsafe { } }\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT(db.AllDiagnostics().empty());
}

WOLANG_TEST(Workspace, ACapabilityGrantLimitsWhatAPackageMayDo) {
  // A sandboxed package is granted a set and nothing more, which is the same
  // mechanism as effects rather than a separate one.
  TempTree tree("sandbox");
  tree.Write("wolang.toml",
             "[package]\nname = \"mod\"\n"
             "[build]\ncapabilities = \"io, alloc\"\n");
  tree.Write("src/main.wo",
             "fun Log() uses io {}\n"
             "fun Grow() uses alloc {}\n"
             "fun Roll() uses rand {}\n"
             "fun Pure() -> int => 1\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));

  const std::vector<Diagnostic> all = db.AllDiagnostics();
  WOLANG_ASSERT_EQ(all.size(), size_t{1});
  WOLANG_EXPECT_EQ(all.front().code, "E0703");
  WOLANG_EXPECT(all.front().message.find("`Roll` needs `rand`") !=
                std::string::npos);
}

WOLANG_TEST(Workspace, AnEmptyGrantIsNotTheSameAsNoGrant) {
  // `capabilities = ""` sandboxes completely; omitting the key leaves the
  // package unrestricted.
  TempTree granted("nogrant");
  granted.Write("wolang.toml", "[package]\nname = \"mod\"\n");
  granted.Write("src/main.wo", "fun Log() uses io {}\n");

  Database open;
  DiagnosticEngine open_diagnostics;
  WOLANG_ASSERT(LoadPackage(&open, granted.path(), &open_diagnostics));
  WOLANG_EXPECT(open.AllDiagnostics().empty());

  TempTree sealed("emptygrant");
  sealed.Write("wolang.toml",
               "[package]\nname = \"mod\"\n"
               "[build]\ncapabilities = \"\"\n");
  sealed.Write("src/main.wo", "fun Log() uses io {}\n");

  Database shut;
  DiagnosticEngine shut_diagnostics;
  WOLANG_ASSERT(LoadPackage(&shut, sealed.path(), &shut_diagnostics));
  WOLANG_EXPECT_EQ(shut.AllDiagnostics().size(), size_t{1});
}

WOLANG_TEST(Workspace, ReportsCyclicPackageDependencies) {
  TempTree tree("cycle");
  tree.Write("a/wolang.toml",
             "[package]\nname = \"a\"\n"
             "[dependencies]\nb = { path = \"../b\" }\n");
  tree.Write("a/src/a.wo", "offer fun A() {}\n");
  tree.Write("b/wolang.toml",
             "[package]\nname = \"b\"\n"
             "[dependencies]\na = { path = \"../a\" }\n");
  tree.Write("b/src/b.wo", "offer fun B() {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  LoadPackage(&db, tree.path("a"), &diagnostics);
  WOLANG_EXPECT(Codes(diagnostics).find("E0059") != std::string::npos);
  // Both packages still loaded, so the rest of the project is usable.
  WOLANG_EXPECT_EQ(db.package_count(), size_t{2});
}

WOLANG_TEST(Workspace, ReportsAMissingManifest) {
  TempTree tree("nomanifest");
  tree.Write("src/main.wo", "fun main() {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_EXPECT(!LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT(Codes(diagnostics).find("E0060") != std::string::npos);
}

WOLANG_TEST(Workspace, ReportsAMissingSourceDirectory) {
  TempTree tree("nosrc");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_EXPECT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT(Codes(diagnostics).find("E0060") != std::string::npos);
}

WOLANG_TEST(Workspace, HonoursACustomSourceDirectory) {
  TempTree tree("custom");
  tree.Write("wolang.toml",
             "[package]\nname = \"game\"\nsource = \"code\"\n");
  tree.Write("code/main.wo", "fun main() {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT(Codes(diagnostics).empty());
  WOLANG_EXPECT_EQ(db.modules().size(), size_t{1});
}

WOLANG_TEST(Workspace, AppliesTheLintPolicyOfThePackage) {
  TempTree tree("lints");
  tree.Write("wolang.toml",
             "[package]\nname = \"game\"\n[lints]\nE0024 = \"off\"\n");
  tree.Write("src/main.wo", "fun main() { let a = * }\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT(db.AllDiagnostics().empty());
}

WOLANG_TEST(Workspace, IsPackageDirectoryDetectsAManifest) {
  TempTree tree("detect");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\n");
  tree.Write("plain/file.wo", "fun f() {}\n");

  WOLANG_EXPECT(IsPackageDirectory(tree.path()));
  WOLANG_EXPECT(!IsPackageDirectory(tree.path("plain")));
  WOLANG_EXPECT(!IsPackageDirectory(tree.path("missing")));
}

}  // namespace
}  // namespace wolang
