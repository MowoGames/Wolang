// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_EMITTER_H_
#define WOLANG_CODEGEN_EMITTER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/codegen/layout.h"
#include "wolang/ir/ir.h"
#include "wolang/ir/trap.h"

namespace wolang {
namespace codegen {

// One place in the generated code that can trap, and what the interpreter
// would have said there. Kept as text rather than as a span so that the two
// tiers' outcomes compare in full: a trap that agrees on the kind but not on
// the place is still a disagreement about the program.
struct TrapSite {
  ir::TrapKind kind = ir::TrapKind::kNone;
  std::string where;
  std::string message;
};

// A `bl` whose target is another function of the same module, which is only
// known once every function has been placed.
struct CallFixup {
  uint32_t word = 0;
  uint32_t callee = 0;
};

struct CompiledFunction {
  bool compiled = false;
  // Why the dev tier left this function to the interpreter. The tier
  // compiles what it can and says what it could not, rather than failing:
  // the interpreter is the reference semantics and always available.
  std::string reason;
  std::vector<uint32_t> words;
  // Where the entry trampoline begins, in words from the start.
  uint32_t stub = 0;
  // Where the function body begins, which is what a `bl` from elsewhere
  // has to reach.
  uint32_t body = 0;
  std::vector<TrapSite> traps;
  std::vector<CallFixup> calls;
};

// Compiles one function to arm64, or says why it cannot.
//
// |compilable| says, per function of the module, whether that callee is
// itself compiled: a call into something this tier cannot take makes the
// caller untakeable too, because there is no way back into the interpreter
// mid-frame.
//
// |trap_site_base| is where this function's trap sites start in the
// module's table, so that a site index means one thing across the module.
//
// |foreign| holds the address of each foreign function of the module, by
// index, or zero for one that is not foreign or whose symbol was not found.
// Resolving a symbol is the host's business, not the emitter's.
CompiledFunction EmitFunction(const StringInterner& interner,
                              const TypeStore& types,
                              LayoutStore* layouts,
                              const ir::Module& module,
                              const ir::Function& function,
                              const std::vector<bool>& compilable,
                              const std::vector<uint64_t>& foreign,
                              uint32_t trap_site_base);

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_EMITTER_H_
