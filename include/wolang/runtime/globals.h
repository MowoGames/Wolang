// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_GLOBALS_H_
#define WOLANG_RUNTIME_GLOBALS_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Where a module-level `var` lives.
//
// Owned by the runtime rather than by either tier, for the reason the
// class tables are: a program may be half interpreted and half compiled,
// and a counter written by one half has to be the counter read by the
// other. The token is the declaration -- `(file << 32) | item` -- so both
// tiers ask for the same storage without anything handing out addresses.
//
// Zeroed on the first ask, which is also the language's rule: a global
// starts as its type's zero and its initializer runs in the module's
// init function.
extern "C" {

void* wolang_global_address(uint64_t token, uint64_t size);

// Forgets everything, for a second compilation in the same process.
void wolang_global_reset(void);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_GLOBALS_H_
