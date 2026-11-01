// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_GLOBALS_H_
#define WOLANG_RUNTIME_GLOBALS_H_

#include <cstdint>
#include <string_view>

#include "wolang/runtime/host.h"

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

// What a vm binds is its own, and so are its host classes and its writer: a
// lookup asks the space this thread has entered first and the process after
// (D257). Threads a program starts inherit the space like its globals.
struct SpaceClass {
  uint64_t Tag = 0;
  HostRetain Retain = nullptr;
  HostRelease Release = nullptr;
  HostAlive Alive = nullptr;
  void* User = nullptr;
  HostToken Token = nullptr;
};

using SpaceWriteHook = int64_t (*)(void* user, int32_t fd, const uint8_t* bytes, uint64_t count);

void SpaceBindSymbol(GlobalSpace* space, std::string_view name, void* address);
void* SpaceSymbol(std::string_view name);
void SpaceRegisterClass(GlobalSpace* space, const SpaceClass& entry);
bool SpaceClassOf(uint64_t tag, SpaceClass* out);
void SpaceCopyBindings(const GlobalSpace* from, GlobalSpace* to);
void SpaceSetWriteHook(GlobalSpace* space, SpaceWriteHook hook, void* user);
bool SpaceWrite(int32_t fd, const uint8_t* bytes, uint64_t count, int64_t* result);

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
