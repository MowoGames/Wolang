// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/workspace.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace wolang {
namespace {

constexpr std::string_view kCyclicDependency = "E0059";
constexpr std::string_view kUnreadablePackage = "E0060";

namespace fs = std::filesystem;

// Exceptions are disabled, so every filesystem call uses its error_code
// overload and failures are reported rather than thrown.
bool Exists(const fs::path& path) {
  std::error_code error;
  return fs::exists(path, error) && !error;
}

bool IsDirectory(const fs::path& path) {
  std::error_code error;
  return fs::is_directory(path, error) && !error;
}

fs::path Canonical(const fs::path& path) {
  std::error_code error;
  const fs::path result = fs::weakly_canonical(path, error);
  return error ? path : result;
}

struct DirectoryContents {
  std::vector<fs::path> files;
  std::vector<fs::path> directories;
};

// Lists one directory with entries sorted, so module membership and file order
// do not depend on the filesystem's iteration order.
DirectoryContents ReadDirectory(const fs::path& directory,
                                std::string_view extension) {
  DirectoryContents contents;
  std::error_code error;
  fs::directory_iterator iterator(directory, error);
  if (error) {
    return contents;
  }

  const fs::directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      break;
    }
    const fs::path& path = iterator->path();
    if (IsDirectory(path)) {
      // Anything starting with a dot is tooling, not source.
      if (!path.filename().string().starts_with(".")) {
        contents.directories.push_back(path);
      }
      continue;
    }
    if (path.extension().string() == extension) {
      contents.files.push_back(path);
    }
  }

  std::sort(contents.files.begin(), contents.files.end());
  std::sort(contents.directories.begin(), contents.directories.end());
  return contents;
}

class Loader {
 public:
  Loader(Database* database,
         DiagnosticEngine* diagnostics,
         const WorkspaceOptions& options)
      : database_(database), diagnostics_(diagnostics), options_(options) {}

  bool Load(const fs::path& directory, bool is_root);

 private:
  void Report(std::string_view code, std::string message) {
    // Filesystem problems have no position to point at.
    diagnostics_->Error(code, std::move(message)).Emit();
  }

  // Registers every directory under |directory| that holds source files as a
  // module named |prefix| plus the path below it.
  void LoadModules(const fs::path& directory,
                   const std::string& module_name,
                   PackageId package);

  Database* database_;
  DiagnosticEngine* diagnostics_;
  WorkspaceOptions options_;
  // Canonical directories already loaded, both to share dependencies between
  // packages and to break cycles.
  std::vector<std::string> loaded_;
  // The chain currently being loaded, for a readable cycle message.
  std::vector<std::string> stack_;
};

void Loader::LoadModules(const fs::path& directory,
                         const std::string& module_name,
                         PackageId package) {
  const DirectoryContents contents =
      ReadDirectory(directory, options_.extension);

  if (!contents.files.empty()) {
    const ModuleId module =
        database_->AddModule(module_name, directory.string(), package);
    std::vector<FileId> files;
    files.reserve(contents.files.size());
    for (const fs::path& path : contents.files) {
      std::string error;
      const FileId file = database_->LoadFile(path.string(), &error);
      if (file.is_valid()) {
        files.push_back(file);
      } else {
        Report(kUnreadablePackage, std::move(error));
      }
    }
    // A module may already have files when two packages share a directory;
    // keep both sets rather than replacing one.
    const Module* existing = database_->GetModule(module);
    if (existing != nullptr && !existing->files.empty()) {
      std::vector<FileId> merged = existing->files;
      for (FileId file : files) {
        if (std::find(merged.begin(), merged.end(), file) == merged.end()) {
          merged.push_back(file);
        }
      }
      files = std::move(merged);
    }
    database_->SetModuleFiles(module, std::move(files));
  }

  for (const fs::path& child : contents.directories) {
    std::string child_name = module_name;
    child_name.push_back('.');
    child_name.append(child.filename().string());
    LoadModules(child, child_name, package);
  }
}

bool Loader::Load(const fs::path& directory, bool is_root) {
  const fs::path canonical = Canonical(directory);
  const std::string key = canonical.string();

  if (std::find(loaded_.begin(), loaded_.end(), key) != loaded_.end()) {
    return true;
  }
  if (std::find(stack_.begin(), stack_.end(), key) != stack_.end()) {
    std::string message = "package dependencies form a cycle through `";
    message.append(key);
    message.append("`");
    Report(kCyclicDependency, std::move(message));
    return false;
  }

  const fs::path manifest_path = canonical / options_.manifest_name;
  if (!Exists(manifest_path)) {
    std::string message = "no `";
    message.append(options_.manifest_name);
    message.append("` in `");
    message.append(key);
    message.append("`");
    Report(kUnreadablePackage, std::move(message));
    return false;
  }

  std::string error;
  const FileId manifest_file = database_->LoadFile(
      manifest_path.string(), &error, FileRole::kAuxiliary);
  if (!manifest_file.is_valid()) {
    Report(kUnreadablePackage, std::move(error));
    return false;
  }

  Manifest manifest =
      ParseManifest(database_->sources(), manifest_file, diagnostics_);
  if (manifest.name.empty()) {
    // ParseManifest already reported why; use the directory so the package
    // still has an identity.
    manifest.name = canonical.filename().string();
  }

  stack_.push_back(key);

  const std::string package_name = manifest.name;
  const std::string source_directory = manifest.source_directory;
  const std::vector<ManifestDependency> dependencies = manifest.dependencies;
  const PackageId package =
      database_->AddPackage(package_name, key, std::move(manifest));

  const fs::path source_root = canonical / source_directory;
  if (IsDirectory(source_root)) {
    LoadModules(source_root, package_name, package);
  } else if (is_root) {
    std::string message = "package `";
    message.append(package_name);
    message.append("` has no source directory `");
    message.append(source_directory);
    message.append("`");
    Report(kUnreadablePackage, std::move(message));
  }

  for (const ManifestDependency& dependency : dependencies) {
    Load(canonical / dependency.path, /*is_root=*/false);
  }

  stack_.pop_back();
  loaded_.push_back(key);
  return true;
}

}  // namespace

bool IsPackageDirectory(std::string_view directory,
                        const WorkspaceOptions& options) {
  const fs::path path(directory);
  return IsDirectory(path) && Exists(path / options.manifest_name);
}

bool LoadPackage(Database* database,
                 std::string_view directory,
                 DiagnosticEngine* diagnostics,
                 const WorkspaceOptions& options) {
  Loader loader(database, diagnostics, options);
  return loader.Load(fs::path(directory), /*is_root=*/true);
}

}  // namespace wolang
