// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SEMA_TYPE_CHECK_H_
#define WOLANG_SEMA_TYPE_CHECK_H_

#include <cstdint>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
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
TypedBody TypeCheckItem(TypeStore* types,
                        const StringInterner* interner,
                        const FileResolution* resolution,
                        const Ast& ast,
                        ItemId item);

}  // namespace wolang

#endif  // WOLANG_SEMA_TYPE_CHECK_H_
