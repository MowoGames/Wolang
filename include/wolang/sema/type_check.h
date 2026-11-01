// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_SEMA_TYPE_CHECK_H_
#define WOLANG_SEMA_TYPE_CHECK_H_

#include <cstdint>
#include <utility>
#include <span>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
#include "wolang/sema/effect.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/type.h"
#include "wolang/syntax/ast.h"

namespace wolang {

// What a body allocates by itself, for the `@Hot` check (D237).
enum class AllocKind : uint8_t {
  kNone,
  kClass,
  kArray,
  kText,
  kError,
  kClosure,
};

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
  // The ones reached through `spawn`. Everything they do carries over --
  // starting work is being responsible for it -- except `async`: the
  // spawner does not suspend.
  std::vector<std::pair<uint32_t, uint32_t>> spawned_callees;
  // Whether the body names a module-level `var` (read or write), and
  // every function it names or calls, declared row or not. The module
  // walk over them finds what a thread body can reach (D232).
  bool touches_global = false;
  std::vector<std::pair<uint32_t, uint32_t>> all_callees;
  // What was handed to a `@Thread` parameter: a function (its callees)
  // or a closure (what its own body touches). Empty when the caller
  // states `uses unsafe`, which takes responsibility for the sharing.
  struct ThreadRoot {
    SourceSpan span;
    bool touches = false;
    std::vector<std::pair<uint32_t, uint32_t>> callees;
  };
  std::vector<ThreadRoot> thread_roots;
  // What one function or method allocates directly (the first site wins) and
  // every function it names or calls, closures and local functions inside it
  // included. `item` is its index in the item tree. Read by the program-wide
  // allocation query, which is what `@Hot` asks (D237).
  struct Allocation {
    uint32_t item = 0xFFFFFFFFu;
    AllocKind kind = AllocKind::kNone;
    Symbol what;
    SourceSpan span;
    std::vector<std::pair<uint32_t, uint32_t>> callees;
  };
  std::vector<Allocation> allocation;
  // Expressions that were accepted as an implicit borrow: an owned
  // `String` where a `str` was wanted (D15b). Recorded because it is the
  // difference between a use that moves and a use that does not, and only
  // the checker knows which rule it applied.
  std::vector<ExprId> borrows;
  // Closures that capture and flow into a kept place: their environment is a
  // counted object on the heap rather than a slot in the frame (D240).
  std::vector<ExprId> escaping;
  bool IsEscaping(ExprId closure) const {
    for (const ExprId id : escaping) {
      if (id == closure) {
        return true;
      }
    }
    return false;
  }
  // `module.Name` expressions that named a module's item rather than a
  // member of a value (D246). Lowering reads this instead of redoing the
  // shadowing rules.
  std::vector<ExprId> qualified;
  // What each closure takes with it: the closure's expression, and the
  // names of the enclosing locals its body read. Lowering builds an
  // environment from them -- in the frame of the function that made the
  // closure, which is why a capturing closure may not outlive it
  // (D138).
  std::vector<std::pair<ExprId, std::vector<Symbol>>> captures;

  // The names |closure| captured, in the order they were first read.
  std::span<const Symbol> CapturesOf(ExprId closure) const {
    for (const auto& [id, names] : captures) {
      if (id == closure) {
        return names;
      }
    }
    return {};
  }

  // What a generic call was inferred to be: the call expression, and the
  // type arguments in the callee's parameter order.
  //
  // Recorded here because inference is what found them and lowering is what
  // needs them: a generic function has no code until it is instantiated, so
  // the IR carries one specialization per entry in this list and none for
  // the generic itself.
  std::vector<std::pair<ExprId, std::vector<TypeId>>> instantiations;

  // An operator the checker settled as a call (D239): `a + b` on a `Vec3`
  // is `a.Plus(b)`. Lowering reads this instead of redoing the lookup, so
  // it sees an ordinary call and the tiers have nothing new to agree on.
  struct OperatorCall {
    ExprId expr;
    OperatorKind kind = OperatorKind::kNone;
    // The type of the left operand, as the checker saw it. A type parameter
    // inside a generic body, which monomorphization replaces.
    TypeId receiver;
    // The method that answers: the implementation's when the operand is
    // concrete, the ability's own when it is a type parameter.
    uint32_t file = Member::kNoSource;
    uint32_t item = Member::kNoSource;
    uint32_t slot = 0;
    TypeId ability;
  };
  std::vector<OperatorCall> operator_calls;

  // The operator call recorded for |id|, or null when it is the built-in one.
  const OperatorCall* OperatorOf(ExprId id) const {
    for (const OperatorCall& call : operator_calls) {
      if (call.expr == id) {
        return &call;
      }
    }
    return nullptr;
  }

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
