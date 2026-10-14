// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SEMA_INITIALIZATION_H_
#define WOLANG_SEMA_INITIALIZATION_H_

#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast.h"

namespace wolang {

// Reports a local read on a path that never wrote to it (`E0123`).
//
// Definite assignment, as a walk over the syntax rather than over the IR.
// It has to be here: the IR is only built for a file that produced no
// errors, so a check that reports one cannot run after lowering. It needs no
// types either -- only names and the shape of the control flow.
//
// What this removes is the one place the two execution tiers disagree: the
// interpreter traps on reading a slot before anything was written, and
// compiled code reads the zero the prologue left. Making the program
// impossible is better than making the tiers agree about it.
void CheckInitialization(const Ast& ast,
                         const StringInterner& interner,
                         ItemId item,
                         DiagnosticEngine* diagnostics);

}  // namespace wolang

#endif  // WOLANG_SEMA_INITIALIZATION_H_
