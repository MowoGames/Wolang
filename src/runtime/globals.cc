// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/globals.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
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
  std::vector<std::pair<std::string, void*>> Symbols;
  std::vector<SpaceClass> Classes;
  SpaceWriteHook WriteHook = nullptr;
  void* WriteUser = nullptr;
};

static GlobalSpace& ProcessSpace() {
  static GlobalSpace sm_Process;
  return sm_Process;
}

static thread_local GlobalSpace* sm_Current = nullptr;

static GlobalSpace& CurrentGlobals() {
  return sm_Current != nullptr ? *sm_Current : ProcessSpace();
}

static void Clear(GlobalSpace& space) {
  for (const GlobalEntry& entry : space.Entries) {
    std::free(entry.storage);
  }
  space.Entries.clear();
}

extern "C" void* wolang_global_address(uint64_t token, uint64_t size) {
  GlobalSpace& space = CurrentGlobals();
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
  Clear(CurrentGlobals());
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

void SpaceBindSymbol(GlobalSpace* space, std::string_view name, void* address) {
  if (space == nullptr) {
    return;
  }
  for (auto& [existing, held] : space->Symbols) {
    if (existing == name) {
      held = address;
      return;
    }
  }
  space->Symbols.emplace_back(std::string(name), address);
}

void* SpaceSymbol(std::string_view name) {
  if (sm_Current == nullptr) {
    return nullptr;
  }
  for (const auto& [existing, held] : sm_Current->Symbols) {
    if (existing == name) {
      return held;
    }
  }
  return nullptr;
}

void SpaceRegisterClass(GlobalSpace* space, const SpaceClass& entry) {
  if (space == nullptr) {
    return;
  }
  for (SpaceClass& held : space->Classes) {
    if (held.Tag == entry.Tag) {
      held = entry;
      return;
    }
  }
  space->Classes.push_back(entry);
}

bool SpaceClassOf(uint64_t tag, SpaceClass* out) {
  if (sm_Current == nullptr) {
    return false;
  }
  for (const SpaceClass& held : sm_Current->Classes) {
    if (held.Tag == tag) {
      *out = held;
      return true;
    }
  }
  return false;
}

void SpaceCopyBindings(const GlobalSpace* from, GlobalSpace* to) {
  if (from == nullptr || to == nullptr) {
    return;
  }
  to->Symbols = from->Symbols;
  to->Classes = from->Classes;
}

void SpaceSetWriteHook(GlobalSpace* space, SpaceWriteHook hook, void* user) {
  if (space == nullptr) {
    return;
  }
  space->WriteHook = hook;
  space->WriteUser = user;
}

bool SpaceWrite(int32_t fd, const uint8_t* bytes, uint64_t count, int64_t* result) {
  if (sm_Current == nullptr || sm_Current->WriteHook == nullptr) {
    return false;
  }
  const int64_t handled = sm_Current->WriteHook(sm_Current->WriteUser, fd, bytes, count);
  if (handled < 0) {
    return false;
  }
  *result = handled;
  return true;
}

}  // namespace runtime
}  // namespace wolang
