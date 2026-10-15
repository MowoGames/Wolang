// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_COMPILER_ITEM_TREE_H_
#define WOLANG_COMPILER_ITEM_TREE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast.h"

namespace wolang {

// One declaration, reduced to what the rest of the program can observe about
// it: its kind, its name, its place in the nesting, and a hash of its shape.
//
// Deliberately absent: source positions and syntax tree handles. Editing a
// function body shifts the offsets of every declaration below it, and adding a
// nested declaration inside a body shifts handles, so storing either would
// make the item tree differ after an edit that changed no signature -- and the
// whole point of the item tree is to compare equal in exactly that case.
// Positions are resolved on demand against the current tree instead.
struct ItemTreeEntry {
  static constexpr uint32_t kNoParent = 0xFFFFFFFFu;

  ItemKind kind = ItemKind::kError;
  ItemFlags flags = ItemFlags::kNone;
  Symbol name;
  uint32_t parent = kNoParent;
  // Hash over the declaration's signature: attributes, modifiers, generics,
  // parameter and return types, base types, effect lists, and the transitions
  // of a state machine. Never over a body or an initializer.
  uint64_t signature = 0;

  friend bool operator==(const ItemTreeEntry& lhs, const ItemTreeEntry& rhs) {
    return lhs.kind == rhs.kind && lhs.flags == rhs.flags &&
           lhs.name == rhs.name && lhs.parent == rhs.parent &&
           lhs.signature == rhs.signature;
  }
};

// The declarations of one file in a deterministic order: pre-order, parents
// before their members, siblings in source order. An entry's index is its
// stable identity, which is what other queries refer to.
struct ItemTree {
  std::vector<ItemTreeEntry> entries;

  friend bool operator==(const ItemTree& lhs, const ItemTree& rhs) {
    return lhs.entries == rhs.entries;
  }
};

// Reduces a parsed file to its declarations.
ItemTree BuildItemTree(const StringInterner& interner, const Ast& ast);

// Returns the declaration that entry |index| describes, walking |ast| in the
// same order BuildItemTree used. Returns an invalid handle when the tree and
// the index disagree, which can only happen if the caller mixes revisions.
ItemId ResolveItem(const Ast& ast, uint32_t index);

// Renders the signature of one declaration as the text its hash is taken
// over. Exposed for tests and for debugging why something was invalidated.
std::string SignatureText(const StringInterner& interner,
                          const Ast& ast,
                          ItemId item);

}  // namespace wolang

#endif  // WOLANG_COMPILER_ITEM_TREE_H_
