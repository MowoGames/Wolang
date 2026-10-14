// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/globals.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace wolang {
namespace runtime {
namespace {

struct Entry {
  uint64_t token = 0;
  void* storage = nullptr;
};

std::vector<Entry>& Entries() {
  static std::vector<Entry> entries;
  return entries;
}

}  // namespace

extern "C" void* wolang_global_address(uint64_t token, uint64_t size) {
  for (const Entry& entry : Entries()) {
    if (entry.token == token) {
      return entry.storage;
    }
  }
  // Never freed: a module-level variable lives as long as the program,
  // which is the one lifetime that needs no bookkeeping.
  void* storage = std::calloc(1, size == 0 ? 1 : size);
  Entries().push_back(Entry{token, storage});
  return storage;
}

extern "C" void wolang_global_reset(void) {
  for (const Entry& entry : Entries()) {
    std::free(entry.storage);
  }
  Entries().clear();
}

}  // namespace runtime
}  // namespace wolang
