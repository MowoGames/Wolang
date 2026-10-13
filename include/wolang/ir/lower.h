// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_IR_LOWER_H_
#define WOLANG_IR_LOWER_H_

#include "wolang/base/string_interner.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/type_check.h"
#include "wolang/syntax/ast.h"

namespace wolang {
namespace ir {

// Lowers one file's declarations into |module|.
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
void LowerFile(TypeStore* types,
               const FileResolution& resolution,
               const Ast& ast,
               const TypedBody& typed,
               Module* module);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_LOWER_H_
