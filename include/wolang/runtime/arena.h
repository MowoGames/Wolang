// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_ARENA_H_
#define WOLANG_RUNTIME_ARENA_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// The allocator an `arena { }` block establishes.
//
// Everything allocated inside the block comes from it, and the whole arena
// goes at the end -- one free for the lot, however many objects and
// strings were built. That is what makes the block worth having: a request
// handler allocates as it likes and pays once.
//
// A stack rather than one arena, because blocks nest. An allocation with
// no block around it falls back to the general heap, so a program that
// never writes `arena` is unaffected.
extern "C" {

void wolang_arena_push();
void wolang_arena_pop();

// Allocates from the innermost arena, or from the heap when there is no
// block around this. Zeroed, like everything else the runtime hands out:
// a field nobody wrote has to read as zero in every tier.
void* wolang_arena_alloc(uint64_t size);

// Whether |pointer| came from an open `arena` block.
//
// What it is for: a destructor running inside one frees storage the arena
// handed out, and the arena is what reclaims that -- so the free has to
// be a no-op rather than a free of something `malloc` never gave out
// (D113).
bool wolang_arena_owns(const void* pointer);

// How deep the arenas are, for a test to be able to see that a block
// released what it took.
uint64_t wolang_arena_depth();
// How many bytes the innermost arena has handed out.
uint64_t wolang_arena_used();

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_ARENA_H_
