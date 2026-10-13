// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_COMPILER_MODULE_H_
#define WOLANG_COMPILER_MODULE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/compiler/item_tree.h"
#include "wolang/compiler/manifest.h"
#include "wolang/syntax/ast.h"

namespace wolang {

struct PackageId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
  friend bool operator==(PackageId lhs, PackageId rhs) {
    return lhs.index == rhs.index;
  }
};

struct ModuleId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
  friend bool operator==(ModuleId lhs, ModuleId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(ModuleId lhs, ModuleId rhs) { return !(lhs == rhs); }
};

// A module is a directory of `.wo` files, and its name is the package name
// followed by the directory path: `src/engine/math.wo` in package `game` is
// part of module `game.engine`.
//
// Directory-scoped modules rather than file-scoped ones mean adding a file
// never requires editing a module declaration, and they match how an engine
// splits a game into plugins.
struct Module {
  std::string name;
  std::string directory;
  PackageId package;
  std::vector<FileId> files;
};

struct Package {
  std::string name;
  std::string directory;
  Manifest manifest;
};

// One name a module declares, pointing at the declaration through its item
// tree index rather than a syntax handle, so the binding survives edits
// elsewhere in the file.
struct ScopeEntry {
  Symbol name;
  FileId file;
  uint32_t entry = 0;
  ItemKind kind = ItemKind::kError;
  // Visible to other modules.
  bool is_public = false;
  // Visible only inside its own file.
  bool is_file_private = false;

  friend bool operator==(const ScopeEntry& lhs, const ScopeEntry& rhs) {
    return lhs.name == rhs.name && lhs.file == rhs.file &&
           lhs.entry == rhs.entry && lhs.kind == rhs.kind &&
           lhs.is_public == rhs.is_public &&
           lhs.is_file_private == rhs.is_file_private;
  }
};

// Everything a module declares, in a deterministic order so that two
// computations of the same module compare equal.
struct ModuleScope {
  std::vector<ScopeEntry> entries;
  // Names declared more than once in this module. The positions are resolved
  // when the diagnostic is produced, not here, because a cached scope must not
  // hold offsets from an older revision.
  std::vector<Symbol> duplicates;

  const ScopeEntry* Find(Symbol name) const;
  bool IsDuplicate(Symbol name) const;

  friend bool operator==(const ModuleScope& lhs, const ModuleScope& rhs) {
    return lhs.entries == rhs.entries && lhs.duplicates == rhs.duplicates;
  }
};

// One `import` declaration after resolution.
struct ResolvedImport {
  // Which `import` of the file this is. An ordinal rather than a syntax handle
  // or an item tree index, because it stays correct no matter what else in the
  // file was edited, and the reporting layer walks the imports in the same
  // order to recover positions.
  uint32_t ordinal = 0;
  // The module the path named, or an invalid id when there is no such module.
  ModuleId module;
  // The names selected by `import a.b.{c, d}`; empty means the whole module.
  std::vector<Symbol> names;
  // Selected names the module does not declare.
  std::vector<Symbol> missing;
  // Selected names the module declares but does not export.
  std::vector<Symbol> private_names;

  friend bool operator==(const ResolvedImport& lhs,
                         const ResolvedImport& rhs) {
    return lhs.ordinal == rhs.ordinal && lhs.module == rhs.module &&
           lhs.names == rhs.names && lhs.missing == rhs.missing &&
           lhs.private_names == rhs.private_names;
  }
};

struct ResolvedImports {
  std::vector<ResolvedImport> imports;
  // True when the file belongs to no module, in which case imports cannot be
  // resolved and nothing is reported: checking a single file outside a package
  // is a legitimate thing to do.
  bool outside_workspace = false;

  friend bool operator==(const ResolvedImports& lhs,
                         const ResolvedImports& rhs) {
    return lhs.imports == rhs.imports &&
           lhs.outside_workspace == rhs.outside_workspace;
  }
};

// Collects the top-level declarations of every file of a module.
ModuleScope BuildModuleScope(const std::vector<FileId>& files,
                             const std::vector<const ItemTree*>& trees);

// Joins an import path into a module name such as "game.engine".
std::string ImportPathText(const StringInterner& interner,
                           const Ast& ast,
                           const Item& item);

}  // namespace wolang

#endif  // WOLANG_COMPILER_MODULE_H_
