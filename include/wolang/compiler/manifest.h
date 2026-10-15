// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_COMPILER_MANIFEST_H_
#define WOLANG_COMPILER_MANIFEST_H_

#include <string>
#include <string_view>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"

namespace wolang {

// A package this one depends on. Only path dependencies exist for now; a
// registry is a later wave.
struct ManifestDependency {
  std::string name;
  std::string path;
  // A dependency fetched rather than found: `git = "..."` with a `rev`,
  // a `tag` or a `branch`. The checkout lands under the workspace's
  // cache and `path` is set to where it landed, so that everything above
  // this sees one kind of dependency (D141).
  std::string git;
  std::string revision;
  // Where inside the checkout the package is, for a repository holding
  // several.
  std::string subdirectory;

  bool is_remote() const { return !git.empty(); }
};

// A per-code severity override from the `[lints]` table.
struct LintOverride {
  std::string code;
  Severity severity = Severity::kWarning;
};

// What `wolang.toml` says about a package.
//
// The file is read with a deliberately small TOML subset: tables, string,
// integer and boolean values, and inline tables. That is everything a package
// manifest needs, and a small parser with precise diagnostics beats a complete
// one with vague ones.
struct Manifest {
  std::string name;
  std::string version;
  // Directory holding the package's modules, relative to the manifest.
  std::string source_directory = "src";
  std::vector<ManifestDependency> dependencies;
  std::vector<LintOverride> lints;
  // `unsafe = "deny"` in `[build]` forbids `unsafe` blocks in this package.
  bool allow_unsafe = true;
  // `capabilities = "io, alloc"` in `[build]` grants this package those
  // effects and nothing else. Empty means unrestricted, which is not the
  // same as granting nothing -- a sandboxed package writes
  // `capabilities = ""`, and `has_capabilities` is what tells them apart.
  std::vector<std::string> capabilities;
  bool has_capabilities = false;
  // What a program using this package has to be linked against, from the
  // `[link]` table: `libraries = "SDL2, m"` becomes `-lSDL2 -lm`,
  // `paths` becomes `-L`, and `frameworks` is what macOS calls a library
  // with headers and resources beside it. A package binding a C library
  // is the package that knows which one, so this travels with it (D129).
  std::vector<std::string> link_libraries;
  std::vector<std::string> link_paths;
  std::vector<std::string> link_frameworks;

  bool is_valid() const { return !name.empty(); }

  // Returns the override for |code|, or |fallback| when there is none.
  Severity SeverityFor(std::string_view code, Severity fallback) const;
};

// Parses an already registered manifest file. Problems are reported through
// |diagnostics| with positions inside the file; the returned manifest holds
// whatever was understood, so one bad line does not discard the rest.
Manifest ParseManifest(const SourceManager& sources,
                       FileId file,
                       DiagnosticEngine* diagnostics);

}  // namespace wolang

#endif  // WOLANG_COMPILER_MANIFEST_H_
