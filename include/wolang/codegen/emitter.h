// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_EMITTER_H_
#define WOLANG_CODEGEN_EMITTER_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/abi/layout.h"
#include "wolang/codegen/target.h"
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

// A call whose target is another function of the same module, which is only
// known once every function has been placed. |at| is where the instruction
// starts, in bytes; what to write there is the target's business, because a
// displacement is a bit field on one machine and four bytes on another.
struct CallFixup {
  uint32_t at = 0;
  uint32_t callee = 0;
};

// Where a function's own address has to be written, for a function used as
// a value. Unlike a call this is an absolute address, so it cannot be
// resolved by placement alone: the block has to know where it will live.
struct AddressFixup {
  uint32_t at = 0;
  uint32_t callee = 0;
};

struct CompiledFunction {
  bool compiled = false;
  // Why the dev tier left this function to the interpreter. The tier
  // compiles what it can and says what it could not, rather than failing:
  // the interpreter is the reference semantics and always available.
  std::string reason;
  // The machine code, as bytes: arm64 instructions are all four bytes wide
  // and x86-64 instructions are between one and fifteen, so the seam counts
  // in the unit they have in common.
  std::vector<uint8_t> bytes;
  // Where the entry trampoline begins, in bytes from the start.
  uint32_t stub = 0;
  // Where the function body begins, which is what a call from elsewhere
  // has to reach.
  uint32_t body = 0;
  std::vector<TrapSite> traps;
  std::vector<CallFixup> calls;
  std::vector<AddressFixup> addresses;
};

// Compiles one function for |target|, or says why it cannot -- which
// includes "there is no backend for this machine", because a tier that
// compiles what it can has to be able to say that too.
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
//
// |strings| holds the address and length of each of the module's string
// literals. The bytes live with the module, so a literal is a pointer to
// them rather than a copy -- which is also why a `str` is a borrow.
CompiledFunction EmitFunction(Target target,
                              const StringInterner& interner,
                              const TypeStore& types,
                              abi::LayoutStore* layouts,
                              const ir::Module& module,
                              const ir::Function& function,
                              const std::vector<bool>& compilable,
                              const std::vector<uint64_t>& foreign,
                              const std::vector<std::pair<uint64_t,
                                                          uint64_t>>& strings,
                              uint32_t trap_site_base);

// Writes the displacement of a call into |code|, now that the callee's
// offset is known. The one piece of placement that cannot be shared: a
// displacement is a bit field inside an instruction word on arm64 and four
// bytes after the opcode on x86-64.
void PatchCall(Target target,
               std::vector<uint8_t>* code,
               uint32_t at,
               uint32_t callee);

// Writes an absolute address into |code|, where a function value was left
// as a placeholder.
void PatchAddress(Target target,
                  std::vector<uint8_t>* code,
                  uint32_t at,
                  uint64_t address);

namespace arm64 {

// The arm64 emitter. Every target has one of these and nothing else.
CompiledFunction EmitFunction(const StringInterner& interner,
                              const TypeStore& types,
                              abi::LayoutStore* layouts,
                              const ir::Module& module,
                              const ir::Function& function,
                              const std::vector<bool>& compilable,
                              const std::vector<uint64_t>& foreign,
                              const std::vector<std::pair<uint64_t,
                                                          uint64_t>>& strings,
                              uint32_t trap_site_base);

}  // namespace arm64

namespace x64 {

// The x86-64 emitter.
CompiledFunction EmitFunction(const StringInterner& interner,
                              const TypeStore& types,
                              abi::LayoutStore* layouts,
                              const ir::Module& module,
                              const ir::Function& function,
                              const std::vector<bool>& compilable,
                              const std::vector<uint64_t>& foreign,
                              const std::vector<std::pair<uint64_t,
                                                          uint64_t>>& strings,
                              uint32_t trap_site_base);

}  // namespace x64

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_EMITTER_H_
