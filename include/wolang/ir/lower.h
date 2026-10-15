// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_LOWER_H_
#define WOLANG_IR_LOWER_H_

#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/systems.h"
#include "wolang/sema/type_check.h"

#include "wolang/syntax/ast.h"

namespace wolang {
namespace ir {

// Lowers one file's declarations into |module|.
//
// |interner| is not const: lowering interns the names of the intrinsics an
// interpolated string needs, which the source never wrote.
//
// Lowering trusts the type checker: it reads the types the checker recorded
// rather than working anything out again, and it is only ever run on a file
// that produced no errors. That division is what keeps the IR free of
// "maybe" cases — every value has a known type by the time it gets here.
//
// Control flow becomes blocks, `match` becomes a switch, and a mutable local
// becomes an `alloca` with loads and stores. The last of those is
// deliberately naive: SROA promotes the slots whose address never escapes,
// and leaving that to a pass keeps lowering simple enough to be obviously
// correct.
// |effects| is what each declaration of this file does, after the
// module-wide fixpoint, prepared by the caller: effects are a module
// question (D36) and lowering is per file, so lowering is told rather than
// asking.
// |file_index| is which file this is, so that a member declared here can be
// told from one declared elsewhere: both are reached through the same member
// table, and only the first has a body in this module.
// What another file's declaration needs in order to be lowered here.
//
// A generic is a recipe, and a copy of it is made where it is *used* --
// which may be a different module from the one it was written in. The copy
// still has to resolve its names the way the original did, so it borrows
// the defining file's syntax, member tables and recorded types.
struct ForeignFile {
  const FileResolution* resolution = nullptr;
  const Ast* ast = nullptr;
  const TypedBody* typed = nullptr;
};

// Answers with an empty ForeignFile for a file that cannot be lowered --
// and then the generic stays a declaration, which the interpreter handles.
using ForeignLookup = std::function<ForeignFile(uint32_t file)>;

// |systems| is the module's schedule, or null when it has none. The
// stages are a module question and lowering is per file, so lowering is
// told rather than asking -- the same arrangement as |effects| (D101).
void LowerFile(StringInterner* interner,
               TypeStore* types,
               uint32_t file_index,
               const FileResolution& resolution,
               const Ast& ast,
               const TypedBody& typed,
               std::span<const std::pair<ItemId, EffectRow>> effects,
               ForeignLookup foreign,
               const AccessGraph* systems,
               Module* module);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_LOWER_H_
