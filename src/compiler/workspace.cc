// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/base/shell.h"
#include "wolang/compiler/workspace.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace wolang {
namespace {

constexpr std::string_view kCyclicDependency = "E0059";
// A dependency that could not be fetched (D141).
constexpr std::string_view kUnfetchable = "E0061";
constexpr std::string_view kUnreadablePackage = "E0060";
// A dependency's version does not meet what a dependent wrote (D242).
constexpr std::string_view kVersionMismatch = "E0053";

namespace fs = std::filesystem;

// The made-up directory of a package a host holds as data (D293).
constexpr std::string_view kMemoryScheme = "memory:";

// How a path or a revision goes into a command line. `cmd.exe` takes only
// double quotes and has no /dev/null, which is why a fetch never got past
// the first `git` on Windows (D286).
#if defined(_WIN32)
constexpr char kQuote = '"';
constexpr std::string_view kDiscardErrors = "2>NUL";
#else
constexpr char kQuote = '\'';
constexpr std::string_view kDiscardErrors = "2>/dev/null";
#endif

std::string ShellQuoted(const std::string& value) {
  std::string quoted(1, kQuote);
  quoted.append(value);
  quoted.push_back(kQuote);
  return quoted;
}

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
  // Seeded from what the database already holds, so that loading two
  // packages that share a dependency loads the dependency once: the
  // second `LoadPackage` is a new Loader, and without this it declares
  // every name in `std` a second time.
  void SeedFromDatabase() {
    for (size_t i = 0; i < database_->package_count(); ++i) {
      const Package* package =
          database_->GetPackage(PackageId{static_cast<uint32_t>(i)});
      if (package == nullptr || package->directory.empty() ||
          package->directory.starts_with(kMemoryScheme)) {
        continue;
      }
      std::error_code failed;
      const fs::path canonical =
          fs::weakly_canonical(fs::path(package->directory), failed);
      loaded_.push_back(failed ? package->directory : canonical.string());
    }
  }

  Loader(Database* database,
         DiagnosticEngine* diagnostics,
         const WorkspaceOptions& options)
      : database_(database), diagnostics_(diagnostics), options_(options) {}

  // The package the load started from: where the cache and the lock file
  // live (D141).
  void SetRoot(const fs::path& root) { root_ = root; }
  const fs::path& root() const { return root_; }

  bool Load(const fs::path& directory, bool is_root);

  bool LoadMemory(std::string_view name,
                  const std::string* manifest_text,
                  const std::vector<MemoryFile>& files);

  // The version in the manifest of the package loaded from |directory|, or
  // empty when there is none.
  std::string VersionAt(const fs::path& directory) const {
    const std::string key = Canonical(directory).string();
    for (size_t i = 0; i < database_->package_count(); ++i) {
      const Package* package =
          database_->GetPackage(PackageId{static_cast<uint32_t>(i)});
      if (package != nullptr && package->directory == key) {
        return package->manifest.version;
      }
    }
    return std::string();
  }

 private:
  // One copy of each package is loaded, so every requirement on it is
  // checked against that copy and the build names the dependent that
  // disagrees. Nothing is resolved (D242).
  void CheckVersion(const std::string& dependent,
                    const ManifestDependency& dependency,
                    const fs::path& found_at) {
    if (dependency.version_req.empty()) {
      return;
    }
    const std::string version = VersionAt(found_at);
    if (VersionSatisfies(dependency.version_req, version)) {
      return;
    }
    std::string message = "`";
    message.append(dependent);
    message.append("` needs `");
    message.append(dependency.name);
    message.append("` ");
    message.append(dependency.version_req);
    message.append(", but the copy found is ");
    message.append(version.empty() ? "unversioned" : version);
    Report(kVersionMismatch, std::move(message));
  }

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
  fs::path root_;
  // Canonical directories already loaded, both to share dependencies between
  // packages and to break cycles.
  std::vector<std::string> loaded_;
  // The chain currently being loaded, for a readable cycle message.
  std::vector<std::string> stack_;

 public:
  // Where a fetched dependency lives: under the cache, named after the
  // repository and the revision. The name is the repository's last part
  // and the revision, which is readable in a listing and unique enough
  // -- two revisions of the same repository are two directories (D141).
  fs::path CheckoutPath(const ManifestDependency& dependency) const {
    const fs::path cache = options_.cache.empty()
                               ? root_ / ".wolang" / "deps"
                               : fs::path(options_.cache);
    std::string name = dependency.git;
    while (!name.empty() && (name.back() == '/' || name.back() == '\n')) {
      name.pop_back();
    }
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0) {
      name = name.substr(0, name.size() - 4);
    }
    std::string revision = dependency.revision;
    for (char& held : revision) {
      if (held == '/' || held == '\\' || held == ':') {
        held = '-';
      }
    }
    fs::path where = cache / (name + "@" + revision);
    if (!dependency.subdirectory.empty()) {
      where /= dependency.subdirectory;
    }
    return where;
  }

  // Clones |dependency| into |checkout| and checks the revision out.
  //
  // Through the `git` on the path rather than through a library: a
  // package manager that cannot be told "use the git you already have"
  // is one that fights the machine it runs on (D141).
  bool Fetch(const ManifestDependency& dependency,
             const fs::path& checkout) {
    // `dir` names a path *inside* the checkout, so the clone goes to
    // wherever that path starts from.
    fs::path top = checkout;
    for (const fs::path& part : fs::path(dependency.subdirectory)) {
      (void)part;
      top = top.parent_path();
    }

    std::error_code failed;
    fs::create_directories(top, failed);
    std::string command = "git clone --quiet --depth 1 ";
    // A revision that is a commit cannot be cloned shallowly by name, so
    // a full clone is the fallback.
    command.append("--branch ");
    command.append(ShellQuoted(dependency.revision));
    command.append(" ");
    command.append(ShellQuoted(dependency.git));
    command.append(" ");
    command.append(ShellQuoted(top.string()));
    command.append(" ");
    command.append(kDiscardErrors);
    int status = std::system(command.c_str());
    if (status != 0) {
      // The failed attempt may have left a directory that a clone refuses
      // to use.
      fs::remove_all(top, failed);
      fs::create_directories(top, failed);
      std::string full = "git clone --quiet ";
      full.append(ShellQuoted(dependency.git));
      full.append(" ");
      full.append(ShellQuoted(top.string()));
      full.append(" ");
      full.append(kDiscardErrors);
      status = std::system(full.c_str());
      if (status == 0) {
        std::string at = "git -C ";
        at.append(ShellQuoted(top.string()));
        at.append(" checkout --quiet ");
        at.append(ShellQuoted(dependency.revision));
        at.append(" ");
        at.append(kDiscardErrors);
        status = std::system(at.c_str());
      }
    }
    if (status != 0) {
      std::string message = "cannot fetch `";
      message.append(dependency.name);
      message.append("` from ");
      message.append(dependency.git);
      Report(kUnfetchable, std::move(message));
      fs::remove_all(top, failed);
      return false;
    }
    return true;
  }

  // The commit a checkout is at, or the revision that was asked for.
  std::string RevisionOf(const fs::path& checkout,
                         const ManifestDependency& dependency) const {
    std::string command = "git -C ";
    command.append(ShellQuoted(checkout.string()));
    command.append(" rev-parse HEAD ");
    command.append(kDiscardErrors);
    FILE* pipe = WOLANG_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
      return dependency.revision;
    }
    char buffer[128] = {};
    const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, pipe);
    WOLANG_PCLOSE(pipe);
    std::string answer(buffer, read);
    while (!answer.empty() &&
           (answer.back() == '\n' || answer.back() == '\r')) {
      answer.pop_back();
    }
    return answer.empty() ? dependency.revision : answer;
  }

 private:
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
    if (dependency.is_remote()) {
      // Fetched rather than found: where the checkout is depends only on
      // the repository and the revision, so two packages that want the
      // same dependency share one copy (D141).
      const fs::path checkout = CheckoutPath(dependency);
      if (!Exists(checkout / options_.manifest_name) && options_.fetch) {
        Fetch(dependency, checkout);
      }
      if (Exists(checkout / options_.manifest_name)) {
        Load(checkout, /*is_root=*/false);
        CheckVersion(package_name, dependency, checkout);
      } else {
        std::string message = "dependency `";
        message.append(dependency.name);
        message.append("` is not fetched yet");
        Report(kUnfetchable, std::move(message));
      }
      continue;
    }
    Load(canonical / dependency.path, /*is_root=*/false);
    CheckVersion(package_name, dependency, canonical / dependency.path);
  }

  stack_.pop_back();
  loaded_.push_back(key);
  return true;
}

// A package a host holds as data (D293). Its name is the host's, not the
// manifest's, so a manifest that disagrees is refused rather than trusted.
bool Loader::LoadMemory(std::string_view name,
                        const std::string* manifest_text,
                        const std::vector<MemoryFile>& files) {
  const std::string package_name(name);
  if (package_name.empty() || package_name == "host") {
    Report(kUnreadablePackage,
           "`" + package_name + "` cannot name a package held in memory");
    return false;
  }

  for (size_t i = 0; i < database_->package_count(); ++i) {
    const Package* held =
        database_->GetPackage(PackageId{static_cast<uint32_t>(i)});
    if (held != nullptr && held->name == package_name) {
      Report(kUnreadablePackage,
             "a package named `" + package_name + "` is already loaded");
      return false;
    }
  }

  const std::string directory = std::string(kMemoryScheme) + package_name;
  Manifest manifest;
  if (manifest_text != nullptr) {
    const FileId manifest_file = database_->AddFile(
        directory + "/" + options_.manifest_name, *manifest_text,
        FileRole::kAuxiliary);
    manifest =
        ParseManifest(database_->sources(), manifest_file, diagnostics_);
    if (!manifest.name.empty() && manifest.name != package_name) {
      Report(kUnreadablePackage,
             "the manifest names `" + manifest.name + "`, but the package is `" +
                 package_name + "`");
      return false;
    }
    manifest.name = package_name;

    for (const ManifestDependency& dependency : manifest.dependencies) {
      const Package* found = nullptr;
      for (size_t i = 0; i < database_->package_count(); ++i) {
        const Package* held =
            database_->GetPackage(PackageId{static_cast<uint32_t>(i)});
        if (held != nullptr && held->name == dependency.name) {
          found = held;
        }
      }
      if (found == nullptr) {
        Report(kUnreadablePackage, "`" + package_name + "` needs `" +
                                       dependency.name +
                                       "`, which is not loaded");
        return false;
      }
      if (!dependency.version_req.empty() &&
          !VersionSatisfies(dependency.version_req, found->manifest.version)) {
        Report(kVersionMismatch,
               "`" + package_name + "` needs `" + dependency.name + "` " +
                   dependency.version_req + ", but the copy found is " +
                   (found->manifest.version.empty()
                        ? std::string("unversioned")
                        : found->manifest.version));
      }
    }
  } else {
    manifest.name = package_name;
    manifest.version = "0.0.0";
    for (size_t i = 0; i < database_->package_count(); ++i) {
      const Package* held =
          database_->GetPackage(PackageId{static_cast<uint32_t>(i)});
      if (held == nullptr) {
        continue;
      }
      ManifestDependency dependency;
      dependency.name = held->name;
      dependency.path = held->directory;
      manifest.dependencies.push_back(std::move(dependency));
    }
  }

  std::string prefix = manifest.source_directory.empty()
                           ? std::string("src")
                           : manifest.source_directory;
  prefix.push_back('/');

  std::vector<std::pair<std::string, const MemoryFile*>> sorted;
  for (const MemoryFile& file : files) {
    std::string path = file.path;
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.starts_with(prefix) || !path.ends_with(options_.extension) ||
        path.size() == prefix.size() + options_.extension.size()) {
      Report(kUnreadablePackage, "`" + file.path + "` is not a `" +
                                     options_.extension + "` file under `" +
                                     prefix + "`");
      return false;
    }
    sorted.emplace_back(std::move(path), &file);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].first == sorted[i - 1].first) {
      Report(kUnreadablePackage, "`" + sorted[i].first + "` is given twice");
      return false;
    }
  }

  const PackageId package =
      database_->AddPackage(package_name, directory, std::move(manifest));

  std::vector<std::pair<std::string, std::vector<FileId>>> modules;
  for (const auto& entry : sorted) {
    const std::string below = entry.first.substr(prefix.size());
    std::string module_name = package_name;
    const size_t slash = below.find_last_of('/');
    if (slash != std::string::npos) {
      std::string parts = below.substr(0, slash);
      std::replace(parts.begin(), parts.end(), '/', '.');
      module_name.push_back('.');
      module_name.append(parts);
    }

    const FileId id =
        database_->AddFile(directory + "/" + entry.first, entry.second->text);
    auto found = std::find_if(
        modules.begin(), modules.end(),
        [&](const auto& held) { return held.first == module_name; });
    if (found == modules.end()) {
      modules.emplace_back(module_name, std::vector<FileId>());
      found = modules.end() - 1;
    }
    found->second.push_back(id);
  }

  for (auto& entry : modules) {
    const ModuleId module =
        database_->AddModule(entry.first, directory + "/" + prefix, package);
    database_->SetModuleFiles(module, std::move(entry.second));
  }
  return true;
}

}  // namespace

bool LoadPackageFromMemory(Database* database,
                           std::string_view name,
                           const std::string* manifest_text,
                           const std::vector<MemoryFile>& files,
                           DiagnosticEngine* diagnostics) {
  Loader loader(database, diagnostics, WorkspaceOptions());
  return loader.LoadMemory(name, manifest_text, files);
}

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
  loader.SeedFromDatabase();
  std::error_code failed;
  loader.SetRoot(fs::weakly_canonical(fs::path(directory), failed));
  return loader.Load(fs::path(directory), /*is_root=*/true);
}

bool FetchDependencies(Database* database,
                       std::string_view directory,
                       DiagnosticEngine* diagnostics,
                       const WorkspaceOptions& options) {
  WorkspaceOptions fetching = options;
  fetching.fetch = true;

  Loader loader(database, diagnostics, fetching);
  loader.SeedFromDatabase();
  std::error_code failed;
  const fs::path root = fs::weakly_canonical(fs::path(directory), failed);
  loader.SetRoot(root);
  if (!loader.Load(fs::path(directory), /*is_root=*/true)) {
    return false;
  }

  // The lock file: every fetched package, the repository it came from,
  // the revision that was asked for and the commit it resolved to. What
  // makes a second machine get the same code (D141).
  std::string text =
      "# Written by `wolang fetch`. Commit this: it is what makes a\n"
      "# second machine build the same code.\n";
  bool complete = true;
  for (size_t i = 0; i < database->package_count(); ++i) {
    const Package* package =
        database->GetPackage(PackageId{static_cast<uint32_t>(i)});
    if (package == nullptr) {
      continue;
    }
    for (const ManifestDependency& dependency :
         package->manifest.dependencies) {
      if (!dependency.is_remote()) {
        continue;
      }
      const fs::path checkout = loader.CheckoutPath(dependency);
      const bool there = Exists(checkout / fetching.manifest_name);
      complete &= there;
      text.append("\n[[package]]\nname = \"");
      text.append(dependency.name);
      text.append("\"\ngit = \"");
      text.append(dependency.git);
      text.append("\"\nrequested = \"");
      text.append(dependency.revision);
      text.append("\"\ncommit = \"");
      text.append(there ? loader.RevisionOf(checkout, dependency)
                        : std::string());
      text.append("\"\nversion = \"");
      text.append(there ? loader.VersionAt(checkout) : std::string());
      text.append("\"\npath = \"");
      text.append(checkout.string());
      text.append("\"\n");
    }
  }

  std::ofstream out((root / "wolang.lock").string(), std::ios::binary);
  if (!out) {
    diagnostics->Error(kUnfetchable, "cannot write wolang.lock").Emit();
    return false;
  }
  out << text;
  return complete;
}

}  // namespace wolang
