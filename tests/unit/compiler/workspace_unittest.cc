// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/compiler/workspace.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "harness/temp_tree.h"
#include "harness/test.h"

namespace wolang {
namespace {

using testing::TempTree;

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
             "fun main() { const _v = Vec3() }\n");
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

WOLANG_TEST(Workspace, ADeniedPackageCannotCallAnUnsafeFun) {
  TempTree tree("denied-unsafe-fun");
  tree.Write("wolang.toml",
             "[package]\nname = \"game\"\n[build]\nunsafe = \"deny\"\n");
  tree.Write("src/main.wo",
             "unsafe fun Poke() {}\n"
             "fun Use() { Poke() }\n");

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
             "[build]\ncapabilities = \"io\"\n");
  tree.Write("src/main.wo",
             "fun Log() uses io {}\n"
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

WOLANG_TEST(Workspace, ChecksAVersionRequirementAgainstTheCopyFound) {
  TempTree tree("version_req");
  tree.Write("game/wolang.toml",
             "[package]\nname = \"game\"\n"
             "[dependencies]\ncore = { path = \"../core\", "
             "version = \"^1.2\" }\n");
  tree.Write("game/src/main.wo", "fun main() {}\n");
  tree.Write("core/wolang.toml",
             "[package]\nname = \"core\"\nversion = \"1.4.0\"\n");
  tree.Write("core/src/log.wo", "offer fun Log() {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path("game"), &diagnostics));
  WOLANG_EXPECT(Codes(diagnostics).empty());
}

WOLANG_TEST(Workspace, ReportsADependencyThatDoesNotMeetItsRequirement) {
  TempTree tree("version_req_bad");
  tree.Write("game/wolang.toml",
             "[package]\nname = \"game\"\n"
             "[dependencies]\ncore = { path = \"../core\", "
             "version = \"^2\" }\n");
  tree.Write("game/src/main.wo", "fun main() {}\n");
  tree.Write("core/wolang.toml",
             "[package]\nname = \"core\"\nversion = \"1.4.0\"\n");
  tree.Write("core/src/log.wo", "offer fun Log() {}\n");

  Database db;
  DiagnosticEngine diagnostics;
  LoadPackage(&db, tree.path("game"), &diagnostics);
  WOLANG_EXPECT_EQ(Codes(diagnostics), "E0053");
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
  tree.Write("src/main.wo", "fun main() { const a = * }\n");

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

WOLANG_TEST(Workspace, RefusesARemoteDependencyThatIsNotFetched) {
  // `check` does not reach the network: a dependency that is not there
  // is an error with a fix in it, not a download (D141).
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "wolang-remote-test";
  std::error_code failed;
  std::filesystem::remove_all(root, failed);
  std::filesystem::create_directories(root / "src", failed);
  {
    std::ofstream manifest((root / "wolang.toml").string());
    manifest << "[package]\nname = \"app\"\nsource = \"src\"\n"
             << "\n[dependencies]\n"
             << "engine = { git = \"https://example.invalid/e.git\", "
                "rev = \"v1\" }\n";
    std::ofstream source((root / "src" / "main.wo").string());
    source << "fun main() {}\n";
  }

  wolang::Database database;
  wolang::DiagnosticEngine diagnostics;
  WOLANG_EXPECT(
      wolang::LoadPackage(&database, root.string(), &diagnostics));
  bool said = false;
  for (const wolang::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    said |= diagnostic.code == "E0061";
  }
  WOLANG_EXPECT(said);
  std::filesystem::remove_all(root, failed);
}

static int RunQuietly(const std::string& command) {
#if defined(_WIN32)
  return std::system((command + " >NUL 2>NUL").c_str());
#else
  return std::system((command + " >/dev/null 2>&1").c_str());
#endif
}

static std::string Slashes(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  return path;
}

static std::string ReadWhole(const std::filesystem::path& path) {
  std::ifstream in(path.string(), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

WOLANG_TEST(Workspace, FetchesAGitDependencyAndLocksIt) {
  // The clone ran through a shell that did not understand its quotes or
  // `/dev/null` on Windows, so no fetch ever got past the first `git` there
  // (D286).
  if (RunQuietly("git --version") != 0) {
    return;
  }

  wolang::testing::TempTree remote("fetch_remote");
  remote.Write("wolang.toml", "[package]\nname = \"math\"\nversion = \"1.4.0\"\n");
  remote.Write("src/math.wo", "offer fun One() -> int => 1\n");
  const std::string git = "git -c user.name=t -c user.email=t@t.invalid -C \"" + std::string(remote.path()) + "\" ";
  WOLANG_ASSERT_EQ(RunQuietly(git + "init --quiet"), 0);
  WOLANG_ASSERT_EQ(RunQuietly(git + "add ."), 0);
  WOLANG_ASSERT_EQ(RunQuietly(git + "commit --quiet -m first"), 0);
  WOLANG_ASSERT_EQ(RunQuietly(git + "tag v1"), 0);

  wolang::testing::TempTree app("fetch_app");
  app.Write("wolang.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nmath = { git = \"" + Slashes(std::string(remote.path())) + "\", tag = \"v1\" }\n");
  app.Write("src/main.wo", "fun main() {}\n");

  wolang::Database database;
  wolang::DiagnosticEngine diagnostics;
  WOLANG_EXPECT(wolang::FetchDependencies(&database, app.path(), &diagnostics));
  WOLANG_EXPECT(diagnostics.diagnostics().empty());

  const std::string lock = ReadWhole(std::filesystem::path(std::string(app.path())) / "wolang.lock");
  WOLANG_EXPECT(lock.find("name = \"math\"") != std::string::npos);
  WOLANG_EXPECT(lock.find("requested = \"v1\"") != std::string::npos);
  WOLANG_EXPECT(lock.find("version = \"1.4.0\"") != std::string::npos);
  const size_t commit = lock.find("commit = \"");
  WOLANG_ASSERT(commit != std::string::npos);
  WOLANG_EXPECT(lock[commit + 10] != '"');
}

WOLANG_TEST(Workspace, ReportsADependencyThatCannotBeFetched) {
  if (RunQuietly("git --version") != 0) {
    return;
  }

  wolang::testing::TempTree app("fetch_missing");
  app.Write("wolang.toml", "[package]\nname = \"app\"\n\n[dependencies]\nmath = { git = \"" + Slashes(std::string(app.path())) + "/no-such-repository\", tag = \"v1\" }\n");
  app.Write("src/main.wo", "fun main() {}\n");

  wolang::Database database;
  wolang::DiagnosticEngine diagnostics;
  WOLANG_EXPECT(!wolang::FetchDependencies(&database, app.path(), &diagnostics));
  bool said = false;
  for (const wolang::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    said |= diagnostic.code == "E0061";
  }
  WOLANG_EXPECT(said);
}

static std::vector<wolang::MemoryFile> Files(
    std::initializer_list<std::pair<const char*, const char*>> given) {
  std::vector<wolang::MemoryFile> files;
  for (const std::pair<const char*, const char*>& file : given) {
    files.push_back(wolang::MemoryFile{file.first, file.second});
  }
  return files;
}

WOLANG_TEST(Workspace, LoadsAPackageFromMemory) {
  wolang::Database db;
  wolang::DiagnosticEngine diagnostics;
  const std::string manifest = "[package]\nname = \"mowo\"\nversion = \"1.2.0\"\n";
  WOLANG_ASSERT(wolang::LoadPackageFromMemory(&db, "mowo", &manifest, Files({{"src/main.wo", "offer fun Top() {}\n"}, {"src/ui/button.wo", "offer fun Click() {}\n"}, {"src/ui/label.wo", "offer fun Text() {}\n"}}), &diagnostics));
  WOLANG_EXPECT(wolang::Codes(diagnostics).empty());

  const wolang::ModuleId root = db.FindModule("mowo");
  const wolang::ModuleId ui = db.FindModule("mowo.ui");
  WOLANG_ASSERT(root.is_valid());
  WOLANG_ASSERT(ui.is_valid());
  WOLANG_EXPECT_EQ(db.GetModule(root)->files.size(), size_t{1});
  WOLANG_EXPECT_EQ(db.GetModule(ui)->files.size(), size_t{2});

  const std::string app_manifest = "[package]\nname = \"app\"\n\n[dependencies]\nmowo = { path = \".\", version = \"^1.1\" }\n";
  WOLANG_ASSERT(wolang::LoadPackageFromMemory(&db, "app", &app_manifest, Files({{"src/main.wo", "need mowo.ui.{Click}\nfun main() { Click() }\n"}}), &diagnostics));
  WOLANG_EXPECT(wolang::Codes(diagnostics).empty());
  WOLANG_EXPECT(db.FindModule("app").is_valid());
  WOLANG_EXPECT_EQ(db.package_count(), size_t{2});
}

WOLANG_TEST(Workspace, ReportsAMemoryPackageDependencyThatIsNotLoaded) {
  wolang::Database db;
  wolang::DiagnosticEngine diagnostics;
  const std::string manifest = "[package]\nname = \"app\"\n\n[dependencies]\nmowo = { path = \".\" }\n";
  WOLANG_EXPECT(!wolang::LoadPackageFromMemory(&db, "app", &manifest, Files({{"src/main.wo", "fun main() {}\n"}}), &diagnostics));
  WOLANG_EXPECT_EQ(wolang::Codes(diagnostics), "E0060");
  WOLANG_EXPECT_EQ(db.package_count(), size_t{0});
}

WOLANG_TEST(Workspace, AMemoryPackageWithoutAManifestDependsOnEverythingLoaded) {
  wolang::Database db;
  wolang::DiagnosticEngine diagnostics;
  WOLANG_ASSERT(wolang::LoadPackageFromMemory(&db, "first", nullptr, Files({{"src/a.wo", "offer fun A() {}\n"}}), &diagnostics));
  WOLANG_ASSERT(wolang::LoadPackageFromMemory(&db, "second", nullptr, Files({{"src/a.wo", "offer fun B() {}\n"}}), &diagnostics));
  WOLANG_EXPECT(wolang::Codes(diagnostics).empty());

  const wolang::Package* second = db.GetPackage(wolang::PackageId{1});
  WOLANG_ASSERT_NE(second, nullptr);
  WOLANG_EXPECT_EQ(second->manifest.dependencies.size(), size_t{1});
  WOLANG_EXPECT_EQ(second->manifest.dependencies[0].name, std::string("first"));
}

WOLANG_TEST(Workspace, RefusesAMemoryPackageWhoseManifestNamesAnother) {
  wolang::Database db;
  wolang::DiagnosticEngine diagnostics;
  const std::string manifest = "[package]\nname = \"other\"\n";
  WOLANG_EXPECT(!wolang::LoadPackageFromMemory(&db, "mowo", &manifest, Files({{"src/a.wo", "offer fun A() {}\n"}}), &diagnostics));
  WOLANG_EXPECT_EQ(wolang::Codes(diagnostics), "E0060");

  wolang::DiagnosticEngine agreeing;
  const std::string fine = "[package]\nname = \"mowo\"\n";
  WOLANG_EXPECT(wolang::LoadPackageFromMemory(&db, "mowo", &fine, Files({{"src/a.wo", "offer fun A() {}\n"}}), &agreeing));
  WOLANG_EXPECT(wolang::Codes(agreeing).empty());
}

WOLANG_TEST(Workspace, RefusesASecondMemoryPackageWithTheSameName) {
  wolang::Database db;
  wolang::DiagnosticEngine diagnostics;
  WOLANG_ASSERT(wolang::LoadPackageFromMemory(&db, "mowo", nullptr, Files({{"src/a.wo", "offer fun A() {}\n"}}), &diagnostics));
  WOLANG_EXPECT(wolang::Codes(diagnostics).empty());
  WOLANG_EXPECT(!wolang::LoadPackageFromMemory(&db, "mowo", nullptr, Files({{"src/b.wo", "offer fun B() {}\n"}}), &diagnostics));
  WOLANG_EXPECT_EQ(wolang::Codes(diagnostics), "E0060");
  WOLANG_EXPECT_EQ(db.package_count(), size_t{1});
}

WOLANG_TEST(Workspace, RefusesTheNameHostForAMemoryPackage) {
  wolang::Database db;
  wolang::DiagnosticEngine diagnostics;
  WOLANG_EXPECT(!wolang::LoadPackageFromMemory(&db, "host", nullptr, Files({{"src/a.wo", "offer fun A() {}\n"}}), &diagnostics));
  WOLANG_EXPECT_EQ(wolang::Codes(diagnostics), "E0060");
}

WOLANG_TEST(Workspace, RefusesAMemoryFileOutsideTheSourceDirectory) {
  wolang::Database db;
  wolang::DiagnosticEngine diagnostics;
  WOLANG_EXPECT(!wolang::LoadPackageFromMemory(&db, "mowo", nullptr, Files({{"lib/a.wo", "offer fun A() {}\n"}}), &diagnostics));
  WOLANG_EXPECT_EQ(wolang::Codes(diagnostics), "E0060");
}
