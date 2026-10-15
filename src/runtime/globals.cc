// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/globals.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace wolang {
namespace runtime {
namespace {

struct GlobalEntry {
  uint64_t token = 0;
  void* storage = nullptr;
};

std::vector<GlobalEntry>& GlobalEntries() {
  static std::vector<GlobalEntry> entries;
  return entries;
}

}  // namespace

extern "C" void* wolang_global_address(uint64_t token, uint64_t size) {
  for (const GlobalEntry& entry : GlobalEntries()) {
    if (entry.token == token) {
      return entry.storage;
    }
  }
  // Never freed: a module-level variable lives as long as the program,
  // which is the one lifetime that needs no bookkeeping.
  void* storage = std::calloc(1, size == 0 ? 1 : size);
  GlobalEntries().push_back(GlobalEntry{token, storage});
  return storage;
}

extern "C" void wolang_global_reset(void) {
  for (const GlobalEntry& entry : GlobalEntries()) {
    std::free(entry.storage);
  }
  GlobalEntries().clear();
}

}  // namespace runtime
}  // namespace wolang
