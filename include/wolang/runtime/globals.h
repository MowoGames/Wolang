// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

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

// A set of module-level `var`s: the process's, or one per embedding vm.
// Entering one makes it the set this thread's code reads and writes, and
// answers the one it replaces, null being the process's (D214).
struct GlobalSpace;
GlobalSpace* GlobalSpaceCreate();
void GlobalSpaceDestroy(GlobalSpace* space);
GlobalSpace* GlobalSpaceEnter(GlobalSpace* space);
GlobalSpace* GlobalSpaceCurrent();

// While it lives, |space| is the current one on this thread.
class GlobalSpaceScope {
 public:
  explicit GlobalSpaceScope(GlobalSpace* space) : m_Previous(GlobalSpaceEnter(space)) {}
  ~GlobalSpaceScope() { GlobalSpaceEnter(m_Previous); }
  GlobalSpaceScope(const GlobalSpaceScope&) = delete;
  GlobalSpaceScope& operator=(const GlobalSpaceScope&) = delete;

 private:
  GlobalSpace* m_Previous;
};

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_GLOBALS_H_
