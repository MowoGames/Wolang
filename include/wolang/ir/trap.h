// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_TRAP_H_
#define WOLANG_IR_TRAP_H_

#include <cstdint>
#include <string_view>

namespace wolang {
namespace ir {

// Why execution stopped. A trap is a defined outcome, not a crash: "this
// program divides by zero" has to be something every tier can be checked
// against, which is only true if the kinds are the same ones.
//
// This belongs to the IR rather than to the interpreter. A division by zero
// is a property of the operation, and both the interpreter and the code
// generator have to name it identically or the differential check between
// them would be comparing two vocabularies.
enum class TrapKind : uint8_t {
  kNone,
  kDivisionByZero,
  kOverflow,
  kOutOfBounds,
  // An optional was read while absent, or an `undef` was used. Both mean the
  // program reached somewhere the front end proved it could not.
  kUndefinedValue,
  kUnreachable,
  // The step budget ran out. Not a property of the program but of this run,
  // and what keeps a `comptime` loop from hanging the compiler.
  kStepLimit,
  // A call to a function with no body.
  kNoBody,
  // An allocation failed. A trap rather than a null handle, because a
  // `class` value is never null and the language has no way to say it is.
  kOutOfMemory,
};

std::string_view TrapKindName(TrapKind kind);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_TRAP_H_
