// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/classes.h"

#include <vector>

namespace wolang {
namespace runtime {
namespace {

struct ClassEntry {
  uint64_t token = 0;
  uint64_t selector = 0;
  void* code = nullptr;
};

// A flat list, searched linearly. There are as many entries as there are
// virtual methods in the program -- tens, in anything written so far --
// and a linear search over tens of 16-byte entries is a cache line or two.
// When that stops being true the fix is a table indexed by slot per
// class, not a hash map: the lookup wants to be inlinable.
std::vector<ClassEntry>& ClassEntries() {
  static std::vector<ClassEntry> entries;
  return entries;
}

}  // namespace

extern "C" void wolang_class_register(uint64_t token, uint64_t selector,
                                      void* code) {
  for (ClassEntry& entry : ClassEntries()) {
    if (entry.token == token && entry.selector == selector) {
      entry.code = code;
      return;
    }
  }
  ClassEntries().push_back(ClassEntry{token, selector, code});
}

extern "C" void* wolang_class_lookup(uint64_t token, uint64_t selector) {
  for (const ClassEntry& entry : ClassEntries()) {
    if (entry.token == token && entry.selector == selector) {
      return entry.code;
    }
  }
  return nullptr;
}

extern "C" void wolang_class_reset(void) { ClassEntries().clear(); }

}  // namespace runtime
}  // namespace wolang
