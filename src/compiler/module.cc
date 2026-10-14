// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/module.h"

#include <algorithm>

namespace wolang {
namespace {

// Declarations that introduce a name other modules can refer to. Imports bind
// nothing of their own, `impl` blocks extend an existing type, and enum
// variants and class members live inside their parent's scope.
bool DeclaresAModuleName(ItemKind kind) {
  switch (kind) {
    case ItemKind::kFunction:
    case ItemKind::kClass:
    case ItemKind::kStruct:
    case ItemKind::kEnum:
    case ItemKind::kAbility:
    case ItemKind::kComponent:
    case ItemKind::kTag:
    case ItemKind::kSystem:
    case ItemKind::kSignal:
    case ItemKind::kBinding:
    case ItemKind::kTypeAlias:
    case ItemKind::kDimension:
    case ItemKind::kUnit:
    case ItemKind::kEffect:
    case ItemKind::kErrorSet:
    case ItemKind::kMachine:
      return true;
    default:
      return false;
  }
}

}  // namespace

const ScopeEntry* ModuleScope::Find(Symbol name) const {
  for (const ScopeEntry& entry : entries) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

bool ModuleScope::IsDuplicate(Symbol name) const {
  return std::find(duplicates.begin(), duplicates.end(), name) !=
         duplicates.end();
}

ModuleScope BuildModuleScope(const std::vector<FileId>& files,
                             const std::vector<const ItemTree*>& trees) {
  ModuleScope scope;
  for (size_t i = 0; i < files.size() && i < trees.size(); ++i) {
    const ItemTree* tree = trees[i];
    if (tree == nullptr) {
      continue;
    }
    for (uint32_t index = 0; index < tree->entries.size(); ++index) {
      const ItemTreeEntry& entry = tree->entries[index];
      if (entry.parent != ItemTreeEntry::kNoParent) {
        continue;
      }
      if (!entry.name.is_valid() || !DeclaresAModuleName(entry.kind)) {
        continue;
      }

      ScopeEntry binding;
      binding.name = entry.name;
      binding.file = files[i];
      binding.entry = index;
      binding.kind = entry.kind;
      binding.is_public = HasFlag(entry.flags, ItemFlags::kOffer);
      binding.is_file_private = HasFlag(entry.flags, ItemFlags::kPriv);
      scope.entries.push_back(binding);
    }
  }

  // A deterministic order is what makes two computations of the same module
  // compare equal, which is what keeps importers green.
  std::sort(scope.entries.begin(), scope.entries.end(),
            [](const ScopeEntry& lhs, const ScopeEntry& rhs) {
              if (lhs.name.index != rhs.name.index) {
                return lhs.name.index < rhs.name.index;
              }
              if (lhs.file.index != rhs.file.index) {
                return lhs.file.index < rhs.file.index;
              }
              return lhs.entry < rhs.entry;
            });

  for (size_t i = 1; i < scope.entries.size(); ++i) {
    if (scope.entries[i].name == scope.entries[i - 1].name &&
        (scope.duplicates.empty() ||
         !(scope.duplicates.back() == scope.entries[i].name))) {
      scope.duplicates.push_back(scope.entries[i].name);
    }
  }
  return scope;
}

std::string ImportPathText(const StringInterner& interner,
                           const Ast& ast,
                           const Item& item) {
  std::string text;
  for (Symbol segment : ast.Get(item.path)) {
    if (!text.empty()) {
      text.push_back('.');
    }
    text.append(interner.Text(segment));
  }
  return text;
}

}  // namespace wolang
