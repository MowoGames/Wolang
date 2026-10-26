// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/globals.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace wolang {
namespace runtime {

struct GlobalEntry {
  uint64_t token = 0;
  uint64_t size = 0;
  void* storage = nullptr;
};

// Where a program's module-level `var`s live. The process has one, and
// an embedding vm has its own, so two vms holding the same program do
// not share them (D214).
struct GlobalSpace {
  std::vector<GlobalEntry> Entries;
};

static GlobalSpace& ProcessSpace() {
  static GlobalSpace sm_Process;
  return sm_Process;
}

static thread_local GlobalSpace* sm_Current = nullptr;

static GlobalSpace& Current() {
  return sm_Current != nullptr ? *sm_Current : ProcessSpace();
}

static void Clear(GlobalSpace& space) {
  for (const GlobalEntry& entry : space.Entries) {
    std::free(entry.storage);
  }
  space.Entries.clear();
}

extern "C" void* wolang_global_address(uint64_t token, uint64_t size) {
  GlobalSpace& space = Current();
  for (const GlobalEntry& entry : space.Entries) {
    if (entry.token == token) {
      return entry.storage;
    }
  }
  // Never freed while the space lives: a module-level variable lives as
  // long as the program, which is the one lifetime that needs no
  // bookkeeping.
  void* storage = std::calloc(1, size == 0 ? 1 : size);
  space.Entries.push_back(GlobalEntry{token, size, storage});
  return storage;
}

extern "C" void wolang_global_reset(void) {
  Clear(Current());
}

GlobalSpace* GlobalSpaceCreate() {
  return new GlobalSpace();
}

void GlobalSpaceDestroy(GlobalSpace* space) {
  if (space == nullptr) {
    return;
  }
  Clear(*space);
  delete space;
}

GlobalSpace* GlobalSpaceEnter(GlobalSpace* space) {
  GlobalSpace* previous = sm_Current;
  sm_Current = space;
  return previous;
}

GlobalSpace* GlobalSpaceCurrent() {
  return sm_Current;
}

}  // namespace runtime
}  // namespace wolang
