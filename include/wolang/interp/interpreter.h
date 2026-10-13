// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_INTERP_INTERPRETER_H_
#define WOLANG_INTERP_INTERPRETER_H_

#include <string>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/interp/value.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace interp {

// Why execution stopped. A trap is a defined outcome, not a crash: the
// interpreter is the reference semantics, so "this program divides by zero"
// has to be something every other tier can be checked against.
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
};

std::string_view TrapKindName(TrapKind kind);

struct Outcome {
  TrapKind trap = TrapKind::kNone;
  // What the function returned, when it returned.
  Value value;
  // Where it stopped, for a trap.
  std::string where;
  std::string message;

  bool ok() const { return trap == TrapKind::kNone; }
};

// How much a run is allowed to do. A `comptime` evaluation runs inside the
// compiler, so it needs a bound; a program run wants a large one.
struct Limits {
  uint64_t steps = 10'000'000;
  uint32_t depth = 1024;
};

// Executes |function| of |module| with |arguments|.
//
// Runs on whatever module it is given. The reference semantics are the
// unoptimized IR; running the optimized one too and comparing is what turns
// "the passes preserve meaning" into something testable rather than assumed.
Outcome Run(const StringInterner& interner,
            const TypeStore& types,
            const ir::Module& module,
            const ir::Function& function,
            std::vector<Value> arguments,
            const Limits& limits = {});

// Convenience: finds |name| in |module| and runs it.
Outcome RunNamed(const StringInterner& interner,
                 const TypeStore& types,
                 const ir::Module& module,
                 Symbol name,
                 std::vector<Value> arguments = {},
                 const Limits& limits = {});

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_INTERPRETER_H_
