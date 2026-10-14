// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SEMA_TYPE_CHECK_H_
#define WOLANG_SEMA_TYPE_CHECK_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
#include "wolang/sema/effect.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/type.h"
#include "wolang/syntax/ast.h"

namespace wolang {

// The type of every expression in one declaration, plus the diagnostics found
// while working them out.
struct TypedBody {
  // Indexed by ExprId. Entries for expressions in other declarations are
  // invalid.
  std::vector<TypeId> expr_types;
  std::vector<Diagnostic> diagnostics;
  // Every file-scope name this declaration referred to, in no particular
  // order and with duplicates. Read to find an import nothing names.
  std::vector<Symbol> used_names;

  // What this declaration's body does by itself: effects from built-in
  // operations and from calls to functions whose row is already declared.
  EffectRow direct_effects;
  // Functions this body calls whose row is not declared, as (file, item)
  // pairs. The module-wide fixpoint needs these: a row cannot be inferred
  // from one body alone.
  std::vector<std::pair<uint32_t, uint32_t>> callees;
  // What a generic call was inferred to be: the call expression, and the
  // type arguments in the callee's parameter order.
  //
  // Recorded here because inference is what found them and lowering is what
  // needs them: a generic function has no code until it is instantiated, so
  // the IR carries one specialization per entry in this list and none for
  // the generic itself.
  std::vector<std::pair<ExprId, std::vector<TypeId>>> instantiations;

  // The item tree index of the declaration this body belongs to, so that a
  // caller elsewhere can be matched to it.
  uint32_t item_index = 0xFFFFFFFFu;

  TypeId TypeOf(ExprId id) const {
    if (!id.is_valid() || id.index >= expr_types.size()) {
      return TypeId{};
    }
    return expr_types[id.index];
  }
};

// Type checks one declaration's signature and body.
//
// Inference is bidirectional: an expected type flows down from the context
// and an inferred type flows up from the expression. That is what lets
// `let x: u8 = 200` give the literal a type without a suffix, and what lets
// `f(1)` take its parameter's type.
// Maps an ItemId to its index in the file's item tree. Built once per file,
// because a type parameter is interned against the declaration that owns it
// and the signature pass and the checker have to agree on which that is.
using ItemIndexMap = std::vector<uint32_t>;

// |item_index| is the declaration's index in its file's item tree, recorded
// in the result so that a caller elsewhere can be matched to it.
TypedBody TypeCheckItem(TypeStore* types,
                        const StringInterner* interner,
                        const FileResolution* resolution,
                        const Ast& ast,
                        ItemId item,
                        uint32_t item_index = 0xFFFFFFFFu,
                        const ItemIndexMap* item_indices = nullptr);

}  // namespace wolang

#endif  // WOLANG_SEMA_TYPE_CHECK_H_
