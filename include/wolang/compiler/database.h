// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_COMPILER_DATABASE_H_
#define WOLANG_COMPILER_DATABASE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/compiler/item_tree.h"
#include "wolang/compiler/manifest.h"
#include "wolang/compiler/module.h"
#include "wolang/ir/ir.h"
#include "wolang/query/query_engine.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/type.h"
#include "wolang/sema/type_check.h"
#include "wolang/syntax/ast.h"
#include "wolang/syntax/token.h"

namespace wolang {

// The result of lexing one file. Diagnostics belong to the result rather than
// to a shared engine: a query may be recomputed at any time, and a cached
// result has to carry its own messages or they would be duplicated or lost.
struct TokenizedFile {
  TokenStream tokens;
  std::vector<Diagnostic> diagnostics;
};

struct ParsedFile {
  Ast ast;
  std::vector<Diagnostic> diagnostics;
};

// What a registered file is for. A manifest lives in the source manager so
// that diagnostics about it can point at a line, but it is not Wolang source
// and must never be lexed as such.
enum class FileRole : uint8_t {
  kSource,
  kAuxiliary,
};

// The identity of a file's text, used as the input value of the SourceText
// query. The text itself stays in the SourceManager; the fingerprint is what
// the dependency graph compares.
struct SourceFingerprint {
  uint64_t hash = 0;
  uint32_t size = 0;
};

// Every declaration of a file, type checked. Bodies are checked per file
// rather than per declaration for now; wave 5 splits this per item so an edit
// to one body does not recheck its neighbours.
struct CheckedFile {
  TypedBody body;
  std::vector<Diagnostic> diagnostics;
  // One entry per top-level declaration, in item tree order.
  std::vector<TypedBody> items;
  // Everything the file can see, with the member tables and signatures of
  // its siblings and imports merged in. Kept here because lowering needs
  // the same view the checker had -- an imported function's signature only
  // exists after the merge.
  FileResolution resolution;
};

// A file's IR, together with whatever the verifier found wrong with it. The
// two travel together because IR that does not verify is a compiler bug, and
// the only useful thing to do with it is show it.
struct LoweredFile {
  ir::Module module;
  std::vector<std::string> problems;
  // Functions carrying `@Test`, by name, in declaration order.
  std::vector<Symbol> tests;
  // Functions carrying `@Bench`. A benchmark is a declaration the driver
  // runs and times, the same way a test is one the driver runs and checks.
  std::vector<Symbol> benches;
  // What `comptime` evaluation found. User-facing, unlike |problems|.
  std::vector<Diagnostic> diagnostics;
  // The IR as lowering left it, before any pass ran. Kept so that a pass
  // that breaks something can be told apart from a lowering that did.
  ir::Module unoptimized;
};


// What every declaration of a module actually does. Computed in one query so
// that a cycle of mutually recursive functions is a loop inside a single
// computation rather than a cycle between queries -- which would make the
// answer depend on which file was asked about first.
struct ModuleEffectRows {
  std::vector<InferredEffects> rows;

  const EffectRow* Find(uint32_t file, uint32_t item) const;

  friend bool operator==(const ModuleEffectRows& lhs,
                         const ModuleEffectRows& rhs) {
    return lhs.rows == rhs.rows;
  }
};

// Owns everything one compilation knows and answers questions about it
// incrementally.
//
// The invariant that makes this sound: a file's text only ever changes through
// SetFileText, which sets the SourceText input and therefore bumps the
// revision. Computations may read the SourceManager directly as long as they
// asked for the matching SourceText query first, which records the dependency.
class Database {
 public:
  Database();
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  SourceManager& sources() { return sources_; }
  const SourceManager& sources() const { return sources_; }
  StringInterner& interner() { return interner_; }
  const StringInterner& interner() const { return interner_; }
  QueryEngine& engine() { return engine_; }
  const QueryEngine& engine() const { return engine_; }
  TypeStore& types() { return types_; }
  const TypeStore& types() const { return types_; }

  // Registers a file and its initial text.
  FileId AddFile(std::string path,
                 std::string text,
                 FileRole role = FileRole::kSource);

  // Reads a file from disk and registers it.
  FileId LoadFile(std::string_view path,
                  std::string* error,
                  FileRole role = FileRole::kSource);

  FileRole RoleOf(FileId file) const;

  // Returns the file registered under |path|, or an invalid id.
  FileId FindFile(std::string_view path) const;

  // Installs an item tree read back from the on-disk cache. The caller must
  // already have checked that the file's text is byte-identical to what the
  // tree was computed from; the tree then stays valid until the file is
  // edited, without the file ever being lexed or parsed.
  void SeedItemTree(FileId file, ItemTree tree);

  // Replaces a file's text. This is the only way a file changes.
  void SetFileText(FileId file, std::string text);

  // --------------------------------------------------------- the workspace --
  //
  // Packages and modules are registered explicitly rather than discovered by a
  // query, because discovery touches the filesystem. Registering one bumps the
  // Workspace input, so anything that resolved a name is invalidated.

  PackageId AddPackage(std::string name,
                       std::string directory,
                       Manifest manifest);
  const Package* GetPackage(PackageId package) const;
  size_t package_count() const { return packages_.size(); }

  ModuleId AddModule(std::string name,
                     std::string directory,
                     PackageId package);
  void SetModuleFiles(ModuleId module, std::vector<FileId> files);

  ModuleId FindModule(std::string_view name) const;
  // The module a file belongs to, or an invalid id for a file that was checked
  // on its own.
  ModuleId ModuleOf(FileId file) const;
  const Module* GetModule(ModuleId module) const;
  const std::vector<Module>& modules() const { return modules_; }

  // ------------------------------------------------------------- questions --

  const TokenizedFile* Tokens(FileId file);
  const ParsedFile* Parse(FileId file);
  const ItemTree* Items(FileId file);
  const ModuleScope* Scope(ModuleId module);
  const ResolvedImports* Imports(FileId file);
  const FileResolution* Resolution(FileId file);
  const CheckedFile* Checked(FileId file);
  const LoweredFile* Lowered(FileId file);

  // Every diagnostic reported for one file, in reporting order.
  const std::vector<Diagnostic>* Diagnostics(FileId file);

  // Convenience for drivers: the diagnostics of every registered file.
  std::vector<Diagnostic> AllDiagnostics();

  size_t file_count() const { return sources_.file_count(); }

 private:
  void RegisterQueries();
  void BumpWorkspace();
  void RecordRole(FileId file, FileRole role);
  // Both resolve their positions against |ast|, which always belongs to the
  // revision being reported.
  void ReportImportProblems(QueryEngine& db,
                            FileId file,
                            const Ast& ast,
                            DiagnosticEngine* reporter);
  void ReportDuplicateDeclarations(QueryEngine& db,
                                   FileId file,
                                   const Ast& ast,
                                   DiagnosticEngine* reporter);
  // The manifest governing a file, or null for a file outside any package.
  const Manifest* ManifestFor(FileId file) const;
  // Applies the package's `[lints]` table, dropping anything turned off.
  void ApplyLintPolicy(FileId file, std::vector<Diagnostic>* diagnostics) const;

  SourceManager sources_;
  StringInterner interner_;
  TypeStore types_;
  QueryEngine engine_;

  // Indexed by FileId.
  std::vector<FileRole> roles_;
  std::vector<Package> packages_;
  std::vector<Module> modules_;
  // Indexed by FileId, so that a file knows its module without a search.
  std::vector<ModuleId> module_of_file_;
  uint64_t workspace_version_ = 0;
};

}  // namespace wolang

#endif  // WOLANG_COMPILER_DATABASE_H_
