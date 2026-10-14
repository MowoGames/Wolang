// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_WORLD_H_
#define WOLANG_RUNTIME_WORLD_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Where components live.
//
// The language has had `component`, `Query` and `system` with a schedule
// since wave 11, and nowhere to put the data: a system was checked and
// laid out in a stage and could not run, because a `Query` had nothing to
// be built from (D100).
//
// The storage is here rather than in Wolang for one reason: a world is a
// map from a component *type* to an array, and the language cannot yet
// spell a map whose keys are types. A token can -- the same
// `(file << 32) | item` number a class carries -- so the runtime holds one
// dense array per token and the language asks for the array it means.
//
// Dense arrays with an entity index: a component lives at the back of its
// own array, an entity's index into it is a lookup, and removing one moves
// the last record into the hole. That is what makes walking every
// `Position` a walk over contiguous bytes, which is the whole point of
// the layout work in D73.
extern "C" {

uint64_t wolang_world_create(void);
void wolang_world_destroy(uint64_t world);

// A new entity, never zero. Zero is "no entity", which is what a query
// cursor answers with when it is past the end.
uint64_t wolang_world_spawn(uint64_t world);
// Removes it and every component it had.
void wolang_world_despawn(uint64_t world, uint64_t entity);
bool wolang_world_alive(uint64_t world, uint64_t entity);
uint64_t wolang_world_count(uint64_t world);

// Storage for |entity|'s component of |token|, |size| bytes, zeroed when
// it is new. Returns where it is, so that the caller writes the value
// through it.
void* wolang_world_add(uint64_t world,
                       uint64_t entity,
                       uint64_t token,
                       uint64_t size);
// Where it is, or null when the entity does not have one.
void* wolang_world_get(uint64_t world, uint64_t entity, uint64_t token);
bool wolang_world_has(uint64_t world, uint64_t entity, uint64_t token);
void wolang_world_remove(uint64_t world, uint64_t entity, uint64_t token);
// How many entities have a component of |token|.
uint64_t wolang_world_component_count(uint64_t world, uint64_t token);

// A walk over the entities that have all of |required| and none of
// |excluded|.
//
// The cursor is a handle rather than a pointer so that it is a number in
// every tier, the way a lock and a thread are (D97). It holds the answer
// it was created with: a query walks the world as it was when the walk
// started, which is what makes adding a component inside a `foreach`
// something that takes effect next time rather than undefined.
uint64_t wolang_query_open(uint64_t world,
                           const uint64_t* required,
                           uint64_t required_count,
                           const uint64_t* excluded,
                           uint64_t excluded_count);
// Moves to the next entity and says whether there is one.
bool wolang_query_next(uint64_t cursor);
// The entity the cursor is on, or zero.
uint64_t wolang_query_entity(uint64_t cursor);
// Where that entity's component of |token| is, or null.
void* wolang_query_component(uint64_t cursor, uint64_t token);
void wolang_query_close(uint64_t cursor);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_WORLD_H_
