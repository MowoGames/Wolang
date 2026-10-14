// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_INTERP_INTERPRETER_H_
#define WOLANG_INTERP_INTERPRETER_H_

#include <string>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/interp/intrinsics.h"
#include "wolang/interp/value.h"
#include "wolang/ir/ir.h"
#include "wolang/ir/trap.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace interp {

// The trap vocabulary is the IR's, not this tier's: the code generator has
// to report the same kinds or the differential check between the two would
// be comparing two vocabularies. See `wolang/ir/trap.h`.
using TrapKind = ir::TrapKind;
using ir::TrapKindName;

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
            const Limits& limits = {},
            Output* output = nullptr);

// Convenience: finds |name| in |module| and runs it.
Outcome RunNamed(const StringInterner& interner,
                 const TypeStore& types,
                 const ir::Module& module,
                 Symbol name,
                 std::vector<Value> arguments = {},
                 const Limits& limits = {},
                 Output* output = nullptr);

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_INTERPRETER_H_
