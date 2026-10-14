// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_IR_LOWER_H_
#define WOLANG_IR_LOWER_H_

#include <span>
#include <utility>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/resolution.h"
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
void LowerFile(StringInterner* interner,
               TypeStore* types,
               const FileResolution& resolution,
               const Ast& ast,
               const TypedBody& typed,
               std::span<const std::pair<ItemId, EffectRow>> effects,
               Module* module);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_LOWER_H_
