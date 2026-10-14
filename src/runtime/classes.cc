// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/classes.h"

#include <vector>

namespace wolang {
namespace runtime {
namespace {

struct Entry {
  uint64_t token = 0;
  uint32_t slot = 0;
  void* code = nullptr;
};

// A flat list, searched linearly. There are as many entries as there are
// virtual methods in the program -- tens, in anything written so far --
// and a linear search over tens of 16-byte entries is a cache line or two.
// When that stops being true the fix is a table indexed by slot per
// class, not a hash map: the lookup wants to be inlinable.
std::vector<Entry>& Entries() {
  static std::vector<Entry> entries;
  return entries;
}

}  // namespace

extern "C" void wolang_class_register(uint64_t token, uint32_t slot,
                                      void* code) {
  for (Entry& entry : Entries()) {
    if (entry.token == token && entry.slot == slot) {
      entry.code = code;
      return;
    }
  }
  Entries().push_back(Entry{token, slot, code});
}

extern "C" void* wolang_class_lookup(uint64_t token, uint32_t slot) {
  for (const Entry& entry : Entries()) {
    if (entry.token == token && entry.slot == slot) {
      return entry.code;
    }
  }
  return nullptr;
}

extern "C" void wolang_class_reset(void) { Entries().clear(); }

}  // namespace runtime
}  // namespace wolang
