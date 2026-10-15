// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_THREADS_H_
#define WOLANG_RUNTIME_THREADS_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Threads, a lock, and one atomic.
//
// The smallest set that makes "run these stages at the same time" and
// "count something from several of them" expressible (D97). Everything
// else -- a thread pool, a channel, a work queue -- is Wolang over these.
//
// The reference tier runs a spawned function *on the spot* instead of
// starting a thread: its values are not addresses and its frames are its
// own, so a second thread inside it would be a second interpreter. That
// makes a program whose answer does not depend on interleaving give the
// same answer in both tiers, which is the only kind of program the
// differential check can compare.
extern "C" {

// Starts |body| with |argument| and returns a handle, or zero.
uint64_t wolang_thread_spawn(void (*body)(void*), void* argument);
// Waits for it. Returns zero on success.
int32_t wolang_thread_join(uint64_t handle);
// How many threads the machine will really run at once.
int32_t wolang_thread_count(void);

// A lock, by handle rather than by address: a Wolang value that is a
// number travels through every tier the same way.
uint64_t wolang_mutex_create(void);
void wolang_mutex_destroy(uint64_t handle);
void wolang_mutex_lock(uint64_t handle);
void wolang_mutex_unlock(uint64_t handle);

// Adds |delta| to the 64-bit number at |address| and returns what was
// there before. The one operation a counter shared between threads needs.
int64_t wolang_atomic_add(int64_t* address, int64_t delta);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_THREADS_H_
