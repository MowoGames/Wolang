// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_CODEGEN_FOREIGN_H_
#define WOLANG_CODEGEN_FOREIGN_H_

#include <memory>

#include "wolang/interp/foreign.h"

namespace wolang {
namespace codegen {

// A foreign caller for the interpreter, built from one generated thunk: it
// loads the argument registers from two arrays and calls through a pointer.
// One thunk covers every signature the C boundary allows, because the only
// thing a signature changes is how many registers hold something -- and
// passing a register the callee does not read costs nothing.
//
// Returns null on a target without a code generator.
std::unique_ptr<interp::ForeignCaller> MakeForeignCaller();

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_FOREIGN_H_
