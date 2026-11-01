// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/compiler/manifest.h"

#include <string>

#include "harness/test.h"

namespace wolang {
namespace {

class Fixture {
 public:
  explicit Fixture(std::string_view text) {
    const FileId file =
        sources_.AddVirtualFile("wolang.toml", std::string(text));
    manifest_ = ParseManifest(sources_, file, &diagnostics_);
  }

  const Manifest& manifest() const { return manifest_; }

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

 private:
  SourceManager sources_;
  DiagnosticEngine diagnostics_;
  Manifest manifest_;
};

WOLANG_TEST(Manifest, ReadsAFullManifest) {
  Fixture fixture(
      "# a game\n"
      "[package]\n"
      "name = \"mygame\"\n"
      "version = \"0.2.1\"\n"
      "source = \"source\"\n"
      "\n"
      "[dependencies]\n"
      "core = { path = \"../core\" }\n"
      "math = { path = \"vendor/math\" }\n"
      "\n"
      "[lints]\n"
      "W0105 = \"off\"\n"
      "P0101 = \"error\"\n"
      "\n"
      "[build]\n"
      "unsafe = \"deny\"\n");

  WOLANG_EXPECT(fixture.Codes().empty());
  const Manifest& manifest = fixture.manifest();
  WOLANG_EXPECT(manifest.is_valid());
  WOLANG_EXPECT_EQ(manifest.name, "mygame");
  WOLANG_EXPECT_EQ(manifest.version, "0.2.1");
  WOLANG_EXPECT_EQ(manifest.source_directory, "source");
  WOLANG_EXPECT(!manifest.allow_unsafe);

  WOLANG_ASSERT_EQ(manifest.dependencies.size(), size_t{2});
  WOLANG_EXPECT_EQ(manifest.dependencies[0].name, "core");
  WOLANG_EXPECT_EQ(manifest.dependencies[0].path, "../core");
  WOLANG_EXPECT_EQ(manifest.dependencies[1].name, "math");
  WOLANG_EXPECT_EQ(manifest.dependencies[1].path, "vendor/math");

  WOLANG_EXPECT_EQ(manifest.SeverityFor("W0105", Severity::kWarning),
                   Severity::kOff);
  WOLANG_EXPECT_EQ(manifest.SeverityFor("P0101", Severity::kPerf),
                   Severity::kError);
  WOLANG_EXPECT_EQ(manifest.SeverityFor("W0101", Severity::kWarning),
                   Severity::kWarning);
}

WOLANG_TEST(Manifest, DefaultsTheSourceDirectory) {
  Fixture fixture("[package]\nname = \"p\"\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.manifest().source_directory, "src");
  WOLANG_EXPECT(fixture.manifest().allow_unsafe);
}

WOLANG_TEST(Manifest, IgnoresCommentsAndBlankLines) {
  Fixture fixture(
      "\n"
      "# leading comment\n"
      "[package]   # trailing\n"
      "name = \"p\" # named p\n"
      "\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.manifest().name, "p");
}

WOLANG_TEST(Manifest, KeepsHashInsideAString) {
  Fixture fixture("[package]\nname = \"p#1\"\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.manifest().name, "p#1");
}

WOLANG_TEST(Manifest, ReportsAMissingPackageTable) {
  Fixture fixture("[dependencies]\ncore = { path = \"../core\" }\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0052");
  WOLANG_EXPECT(!fixture.manifest().is_valid());
}

WOLANG_TEST(Manifest, ReportsAMissingName) {
  Fixture fixture("[package]\nversion = \"1.0\"\n");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0052");
}

WOLANG_TEST(Manifest, ReportsSyntaxProblems) {
  WOLANG_EXPECT_EQ(Fixture("[package\nname = \"p\"\n").Codes(),
                   "E0050,E0050,E0052");
  WOLANG_EXPECT(Fixture("[package]\nname\n").Codes().find("E0050") !=
                std::string::npos);
  WOLANG_EXPECT(Fixture("[package]\nname =\n").Codes().find("E0050") !=
                std::string::npos);
  WOLANG_EXPECT(Fixture("name = \"p\"\n").Codes().find("E0050") !=
                std::string::npos);
}

WOLANG_TEST(Manifest, ReportsUnknownKeysAndTables) {
  WOLANG_EXPECT(Fixture("[package]\nname = \"p\"\nauthor = \"me\"\n")
                    .Codes()
                    .find("E0051") != std::string::npos);
  WOLANG_EXPECT(Fixture("[package]\nname = \"p\"\n[nonsense]\na = \"b\"\n")
                    .Codes()
                    .find("E0051") != std::string::npos);
}

WOLANG_TEST(Manifest, ReportsInvalidValues) {
  WOLANG_EXPECT(Fixture("[package]\nname = p\n").Codes().find("E0053") !=
                std::string::npos);
  WOLANG_EXPECT(
      Fixture("[package]\nname = \"p\"\n[lints]\nW0105 = \"loud\"\n")
          .Codes()
          .find("E0053") != std::string::npos);
  WOLANG_EXPECT(
      Fixture("[package]\nname = \"p\"\n[build]\nunsafe = \"maybe\"\n")
          .Codes()
          .find("E0053") != std::string::npos);
}

WOLANG_TEST(Manifest, ReportsDuplicateKeys) {
  Fixture fixture("[package]\nname = \"a\"\nname = \"b\"\n");
  WOLANG_EXPECT(fixture.Codes().find("E0054") != std::string::npos);
  // The first value wins, so the rest of the manifest still makes sense.
  WOLANG_EXPECT_EQ(fixture.manifest().name, "a");
}

WOLANG_TEST(Manifest, ReportsDependenciesWithoutAPath) {
  WOLANG_EXPECT(
      Fixture("[package]\nname = \"p\"\n[dependencies]\ncore = { }\n")
          .Codes()
          .find("E0052") != std::string::npos);
  // A revision with nothing to fetch from is as incomplete as no path at
  // all (D141).
  WOLANG_EXPECT(Fixture("[package]\nname = \"p\"\n[dependencies]\n"
                        "core = { branch = \"main\" }\n")
                    .Codes()
                    .find("E0052") != std::string::npos);
}

WOLANG_TEST(Manifest, ReadsARemoteDependency) {
  Fixture fixture(
      "[package]\nname = \"p\"\n[dependencies]\n"
      "engine = { git = \"https://example.com/engine.git\", "
      "rev = \"v1.2.0\", dir = \"lib/engine\" }\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.manifest().dependencies.size(), 1u);
  const wolang::ManifestDependency& held =
      fixture.manifest().dependencies.front();
  WOLANG_EXPECT_EQ(held.name, "engine");
  WOLANG_EXPECT_EQ(held.git, "https://example.com/engine.git");
  WOLANG_EXPECT_EQ(held.revision, "v1.2.0");
  WOLANG_EXPECT_EQ(held.subdirectory, "lib/engine");
  WOLANG_EXPECT(held.is_remote());
}

WOLANG_TEST(Manifest, RefusesARepositoryWithNoRevision) {
  // A dependency with no revision changes under you, and the lock file
  // has nothing to record (D141).
  WOLANG_EXPECT(Fixture("[package]\nname = \"p\"\n[dependencies]\n"
                        "engine = { git = \"https://e.com/e.git\" }\n")
                    .Codes()
                    .find("E0052") != std::string::npos);
}

WOLANG_TEST(Manifest, AllocIsNotACapability) {
  WOLANG_EXPECT_EQ(Fixture("[package]\nname = \"p\"\n[build]\ncapabilities = \"io, alloc\"\n").Codes(), "E0053");
  WOLANG_EXPECT_EQ(Fixture("[package]\nname = \"p\"\n[build]\ncapabilities = \"io\"\n").Codes(), "");
}

WOLANG_TEST(Manifest, ReadsAVersionRequirementWithCommasInQuotes) {
  // The inline table is split on commas, but not the ones in quotes (D242).
  Fixture fixture("[package]\nname = \"p\"\n[dependencies]\n"
                  "core = { path = \"../core\", version = \">=1.2, <2\" }\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_ASSERT_EQ(fixture.manifest().dependencies.size(), 1u);
  WOLANG_EXPECT_EQ(fixture.manifest().dependencies[0].version_req,
                   ">=1.2, <2");
  WOLANG_EXPECT_EQ(fixture.manifest().dependencies[0].path, "../core");
}

WOLANG_TEST(Manifest, RefusesABadVersionRequirement) {
  WOLANG_EXPECT(Fixture("[package]\nname = \"p\"\n[dependencies]\n"
                        "core = { path = \"../core\", version = \"^x\" }\n")
                    .Codes() == "E0053");
}

WOLANG_TEST(Manifest, RefusesABareStringDependency) {
  // A bare string is a version everywhere else, and there is no registry
  // (D242).
  WOLANG_EXPECT(Fixture("[package]\nname = \"p\"\n[dependencies]\n"
                        "core = \"1.2\"\n")
                    .Codes() == "E0053");
}

WOLANG_TEST(Manifest, VersionRequirementsFollowCargo) {
  using wolang::VersionSatisfies;
  WOLANG_EXPECT(VersionSatisfies("^1.2", "1.9.0"));
  WOLANG_EXPECT(VersionSatisfies("1.2", "1.2.5"));
  WOLANG_EXPECT(!VersionSatisfies("^1.2", "2.0.0"));
  WOLANG_EXPECT(!VersionSatisfies("^1.2", "1.1"));
  WOLANG_EXPECT(VersionSatisfies("^0.2.3", "0.2.9"));
  WOLANG_EXPECT(!VersionSatisfies("^0.2.3", "0.3.0"));
  WOLANG_EXPECT(!VersionSatisfies("^0.0.3", "0.0.4"));
  WOLANG_EXPECT(VersionSatisfies("~1.2", "1.2.9"));
  WOLANG_EXPECT(!VersionSatisfies("~1.2", "1.3.0"));
  WOLANG_EXPECT(VersionSatisfies("=1.2.3", "1.2.3"));
  WOLANG_EXPECT(!VersionSatisfies("=1.2.3", "1.2.4"));
  WOLANG_EXPECT(VersionSatisfies(">=1, <2", "1.5"));
  WOLANG_EXPECT(!VersionSatisfies(">=1, <2", "2.0"));
  WOLANG_EXPECT(VersionSatisfies("*", "0.0.1"));
  WOLANG_EXPECT(!wolang::IsValidVersionReq(""));
  WOLANG_EXPECT(!wolang::IsValidVersionReq(">=1,"));
}

WOLANG_TEST(Manifest, KeepsParsingAfterABadLine) {
  Fixture fixture(
      "[package]\n"
      "name = \"p\"\n"
      "oops\n"
      "version = \"1.2.3\"\n");
  WOLANG_EXPECT(fixture.Codes().find("E0050") != std::string::npos);
  WOLANG_EXPECT_EQ(fixture.manifest().name, "p");
  WOLANG_EXPECT_EQ(fixture.manifest().version, "1.2.3");
}

}  // namespace
}  // namespace wolang
