// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_PASSES_H_
#define WOLANG_IR_PASSES_H_

#include "wolang/abi/layout.h"
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

// Replaces a call to a small function with the body it calls.
//
// Inlines one call per invocation, so a driver can run it to a fixpoint
// alongside everything else. The rewrite needs no renumbering: the call
// instruction *becomes* the parameter of the block control returns to, so
// every operand that read the call's result reads the parameter instead
// without being touched.
bool InlineCalls(TypeStore* types, const Module& module, Function* function);

// Replaces a jump to a block with one predecessor by that block's contents,
// and drops blocks nothing reaches.
bool SimplifyControlFlow(TypeStore* types, Function* function);

// Runs every pass to a fixpoint.
// One copy of each computation per block: a read and a write to the same
// place work the address out once.
bool EliminateCommonSubexpressions(TypeStore* types, Function* function);

// Moves what does not change out of the loop that computes it. Only what
// cannot trap: a check hoisted out of a loop that never runs would trap
// where the program would not have.
bool HoistLoopInvariants(TypeStore* types, Function* function);

// Turns an element address into the arithmetic it is, now that the layout
// is known: what a loop does per element becomes a multiply and an add,
// and everything else is hoisted out by the passes that follow.
bool ExpandElementAddresses(TypeStore* types,
                            abi::LayoutStore* layouts,
                            Function* function);

// |layouts| may be null, and then addresses stay as they are: a caller
// that has no layout -- a test over hand-written IR -- still gets the rest
// of the passes.
void Optimize(TypeStore* types,
              const Module& module,
              Function* function,
              abi::LayoutStore* layouts = nullptr);
void Optimize(TypeStore* types,
              Module* module,
              abi::LayoutStore* layouts = nullptr);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_PASSES_H_
