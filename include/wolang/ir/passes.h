// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_IR_PASSES_H_
#define WOLANG_IR_PASSES_H_

#include "wolang/ir/ir.h"

namespace wolang {
namespace ir {

// Each pass rewrites one function in place and reports whether it changed
// anything, so a driver can run them to a fixpoint. Every pass has to leave
// the function verifiable: a pass that produces IR the verifier rejects is a
// compiler bug, and the tests check that rather than trusting it.
//
// None of them renumber instructions. A dead instruction is turned into an
// `undef` that nothing uses rather than removed, because an index is a name
// here -- compacting would mean rewriting every operand, and the back ends
// skip values with no uses anyway.

// Replaces an operation on constants with the constant it produces.
bool FoldConstants(TypeStore* types, Function* function);

// Removes instructions nothing uses and that cannot have an effect.
bool EliminateDeadCode(TypeStore* types,
                       const Module& module,
                       Function* function);

// Promotes a slot whose address never escapes into SSA values, which is what
// lets lowering put every `var` in an `alloca` without paying for it.
bool PromoteSlots(TypeStore* types, Function* function);

// Replaces a jump to a block with one predecessor by that block's contents,
// and drops blocks nothing reaches.
bool SimplifyControlFlow(TypeStore* types, Function* function);

// Runs every pass to a fixpoint.
void Optimize(TypeStore* types, const Module& module, Function* function);
void Optimize(TypeStore* types, Module* module);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_PASSES_H_
