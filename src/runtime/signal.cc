// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/signal.h"

#include <cstring>

#include "wolang/runtime/arena.h"

namespace wolang {
namespace runtime {

void wolang_signal_connect(SignalList* list, void* handler) {
  if (list == nullptr || handler == nullptr) {
    return;
  }
  if (list->count == list->capacity) {
    // Four to begin with and double after: a signal usually has one or two
    // listeners, and the ones that have many have them from the start.
    const uint64_t wanted = list->capacity == 0 ? 4 : list->capacity * 2;
    auto** grown = static_cast<void**>(
        wolang_arena_alloc(wanted * sizeof(void*)));
    if (grown == nullptr) {
      return;
    }
    if (list->count != 0 && list->handlers != nullptr) {
      std::memcpy(grown, list->handlers,
                  static_cast<size_t>(list->count) * sizeof(void*));
    }
    // The old block is not freed: it came from an arena or from the heap,
    // and nothing frees either yet. A signal connected in a loop forever
    // would grow forever, which is said out loud here rather than
    // discovered.
    list->handlers = grown;
    list->capacity = wanted;
  }
  list->handlers[list->count++] = handler;
}

bool wolang_signal_disconnect(SignalList* list, void* handler) {
  if (list == nullptr || list->handlers == nullptr) {
    return false;
  }
  for (uint64_t i = 0; i < list->count; ++i) {
    if (list->handlers[i] != handler) {
      continue;
    }
    // The order of the rest is kept: handlers run in the order they were
    // connected, and a disconnect in the middle must not shuffle them.
    for (uint64_t j = i + 1; j < list->count; ++j) {
      list->handlers[j - 1] = list->handlers[j];
    }
    --list->count;
    return true;
  }
  return false;
}

}  // namespace runtime
}  // namespace wolang
