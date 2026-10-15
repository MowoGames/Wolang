// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_SEMA_FLOW_H_
#define WOLANG_SEMA_FLOW_H_

#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/type.h"
#include "wolang/sema/type_check.h"
#include "wolang/syntax/ast.h"

namespace wolang {

// What one declaration does to its locals, as a walk over the syntax:
// whether every path to a use wrote the variable (`E0123`), and whether a
// value that was moved away is used again (`E0122`).
//
// Both have to be here rather than on the IR: the IR is only built for a
// file that produced no errors, so a check that reports one cannot run
// after lowering.
//
// `E0123` removes the one place the two execution tiers disagreed: the
// interpreter traps on reading a slot before anything was written, and
// compiled code reads the zero the prologue left. Making the program
// impossible is better than making the tiers agree about it.
//
// `E0122` needs the types the checker recorded, and only those: whether a
// use of a name is a move or a borrow is exactly the question "what type
// did the context want" -- `String` moves, `str` borrows (D15b).
// |resolution| is what says which types own something: a type with a
// `deinit` is moved rather than copied, because two names holding one
// resource is the bug destructors exist to make impossible (D87).
void CheckFlow(const Ast& ast,
               const StringInterner& interner,
               const TypeStore& types,
               const FileResolution& resolution,
               const TypedBody& typed,
               ItemId item,
               DiagnosticEngine* diagnostics);

}  // namespace wolang

#endif  // WOLANG_SEMA_FLOW_H_
