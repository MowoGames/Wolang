// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_SIGNAL_H_
#define WOLANG_RUNTIME_SIGNAL_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// What a signal holds, as the compiler lays it out: how many handlers
// there are, how many there is room for, and where they are.
//
// The growing is here rather than in generated code because it is the one
// part that allocates, and because every tier then grows a list the same
// way. Emitting is *not* here: calling back into the language is something
// only the language can do, so the loop over handlers is generated.
struct SignalList {
  uint64_t count = 0;
  uint64_t capacity = 0;
  void** handlers = nullptr;
};

extern "C" {

// Adds |handler| to the list at |list|. Nothing is checked for
// duplicates: connecting the same handler twice calls it twice, which is
// what every signal library does and what every user eventually relies
// on.
void wolang_signal_connect(SignalList* list, void* handler);

// Removes the first occurrence of |handler|, and returns whether it was
// there.
bool wolang_signal_disconnect(SignalList* list, void* handler);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_SIGNAL_H_
