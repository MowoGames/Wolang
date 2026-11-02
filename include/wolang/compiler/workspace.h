// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_COMPILER_WORKSPACE_H_
#define WOLANG_COMPILER_WORKSPACE_H_

#include <string>
#include <string_view>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/compiler/database.h"

namespace wolang {

struct WorkspaceOptions {
  // The manifest file that marks a package root.
  std::string manifest_name = "wolang.toml";
  // The extension of a source file.
  std::string extension = ".wo";
  // Whether a dependency that is not there yet may be fetched.
  //
  // Off for the commands that only read -- `check` should not reach the
  // network because a file was saved -- and on for `fetch` and for the
  // commands a person waits on anyway (D141).
  bool fetch = false;
  // Where fetched packages are kept. Empty means `<root>/.wolang/deps`.
  std::string cache;
};

// Loads the package rooted at |directory| into |database|, together with the
// packages it depends on.
//
// Discovery is deliberately not a query: it touches the filesystem, and the
// query engine only tracks facts it can be told about. Reloading a workspace
// bumps the Workspace input, so everything that resolved a name is
// invalidated.
//
// Returns false when the package itself could not be read. Problems inside it
// are reported through |diagnostics| and do not stop the load, so a project
// with one broken dependency still opens.
bool LoadPackage(Database* database,
                 std::string_view directory,
                 DiagnosticEngine* diagnostics,
                 const WorkspaceOptions& options = {});

// Fetches every remote dependency of the package rooted at |directory|,
// writes `wolang.lock` beside the manifest, and answers whether all of
// it worked.
//
// Fetching is `git`: a repository, a revision, and a checkout under the
// workspace's cache. Which is the smallest thing that can be called a
// package manager and still be reproducible -- the lock file records the
// commit each dependency resolved to, so a second machine gets the same
// code (D141).
bool FetchDependencies(Database* database,
                       std::string_view directory,
                       DiagnosticEngine* diagnostics,
                       const WorkspaceOptions& options = {});

// One source file of a package that exists only in memory: its path below the
// package root (`src/ui/button.wo`) and its text.
struct MemoryFile {
  std::string path;
  std::string text;
};

// Loads a package a host holds as data. It is named |name|, lives in a made-up
// directory `memory:<name>`, and its modules are named from the paths of
// |files| as they would be on disk: `src/ui/button.wo` is in `<name>.ui`.
//
// |manifest_text| is a manifest, trusted like an on-disk package's (`unsafe`
// and `[link]` allowed); its dependencies are found by name among the packages
// already loaded, so a path in one is only a placeholder. A null manifest means
// the package depends on everything loaded, as `host` does (D293).
//
// Returns false, with a diagnostic, when the package was refused: the manifest
// names another package, the name is `host` or already loaded, a dependency is
// not loaded, or a path is not a source file under the source directory.
bool LoadPackageFromMemory(Database* database,
                           std::string_view name,
                           const std::string* manifest_text,
                           const std::vector<MemoryFile>& files,
                           DiagnosticEngine* diagnostics);

// True when |directory| holds a package manifest.
bool IsPackageDirectory(std::string_view directory,
                        const WorkspaceOptions& options = {});

}  // namespace wolang

#endif  // WOLANG_COMPILER_WORKSPACE_H_
