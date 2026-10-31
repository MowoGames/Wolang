// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/world.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "wolang/runtime/threads.h"

namespace wolang {
namespace runtime {
namespace {

// One component type's records, side by side.
struct Column {
  uint64_t size = 0;
  std::vector<uint8_t> records;
  // Which record each entity's component is, and which entity each record
  // belongs to. Both, because removing one moves the last record into the
  // hole and the entity that owned it has to be told where it went.
  std::unordered_map<uint64_t, uint64_t> of_entity;
  std::vector<uint64_t> owners;

  uint64_t Count() const { return owners.size(); }
  uint8_t* At(uint64_t index) { return records.data() + index * size; }
};

struct World {
  uint64_t next_entity = 1;
  std::unordered_map<uint64_t, Column> columns;
  std::vector<uint64_t> living;
  std::unordered_map<uint64_t, bool> alive;
};

// A walk, with the entities it answered with already chosen.
//
// The columns are resolved once, when the walk starts, and the component
// a step is on is worked out then too: a `foreach` asks for the address
// of a component once per entity, and a lookup per ask was most of the
// cost of running a system (D101).
struct Cursor {
  World* world = nullptr;
  std::vector<uint64_t> entities;
  // One past the entity the cursor is on; zero means "before the first".
  size_t at = 0;

  struct Held {
    uint64_t token = 0;
    Column* column = nullptr;
    // Where this component of the current entity is, or null.
    uint8_t* address = nullptr;
    // Which step that address was worked out for, so that a component
    // nothing asks about costs nothing.
    size_t located = 0;
  };
  std::vector<Held> held;
  // The column the walk goes along, and where each entity's record is in
  // it: the first component the query named. Walking it by index is what
  // makes the common case -- a system that reads the component it is
  // walking -- cost no lookup at all (D101).
  Column* walked = nullptr;
  std::vector<uint64_t> records;
  // Where each matching entity's walked component is, worked out when the
  // walk opens.
  //
  // Built up front so that the loop itself is arithmetic: a call per
  // entity was most of what running a system cost, and a hundred
  // thousand entities is a hundred thousand calls. One pointer each is
  // the price, paid once per query rather than per component read.
  std::vector<void*> addresses;

  // Where the walked component of the current entity is, by index rather
  // than by lookup.
  uint8_t* Current() {
    if (walked == nullptr || at == 0 || at > records.size()) {
      return nullptr;
    }
    return walked->At(records[at - 1]);
  }

  // Where another of the current entity's components is. Worked out when
  // something asks, and only once per step.
  uint8_t* Locate(uint64_t token) {
    if (at == 0 || at > entities.size()) {
      return nullptr;
    }
    for (Held& one : held) {
      if (one.token != token) {
        continue;
      }
      if (one.column == walked) {
        return Current();
      }
      if (one.located == at) {
        return one.address;
      }
      one.located = at;
      one.address = nullptr;
      if (one.column != nullptr) {
        const auto found = one.column->of_entity.find(entities[at - 1]);
        if (found != one.column->of_entity.end()) {
          one.address = one.column->At(found->second);
        }
      }
      return one.address;
    }
    return nullptr;
  }
};

std::mutex& WorldLock() {
  static std::mutex lock;
  return lock;
}

std::vector<World*>& Worlds() {
  static std::vector<World*> worlds;
  return worlds;
}

// The systems of the stage being put together, on this thread: a stage is
// added to and then run, and two threads building one at the same time
// would be two schedules.
std::vector<void*>& Stage() {
  static thread_local std::vector<void*> stage;
  return stage;
}

// Cursors belong to the thread that opened them: a system opens its own
// queries and walks them itself, so there is nothing to share -- and a
// table that is not shared needs no lock, which is what a per-entity
// lookup cannot afford (D101).
std::vector<Cursor*>& Cursors() {
  static thread_local std::vector<Cursor*> cursors;
  return cursors;
}

World* Find(uint64_t handle) {
  const std::lock_guard<std::mutex> held(WorldLock());
  if (handle == 0 || handle > Worlds().size()) {
    return nullptr;
  }
  return Worlds()[handle - 1];
}

Cursor* FindCursor(uint64_t handle) {
  if (handle == 0 || handle > Cursors().size()) {
    return nullptr;
  }
  return Cursors()[handle - 1];
}

}  // namespace

extern "C" uint64_t wolang_world_create(void) {
  auto* world = new World();
  const std::lock_guard<std::mutex> held(WorldLock());
  Worlds().push_back(world);
  return Worlds().size();
}

extern "C" void wolang_world_destroy(uint64_t handle) {
  const std::lock_guard<std::mutex> held(WorldLock());
  if (handle == 0 || handle > Worlds().size()) {
    return;
  }
  delete Worlds()[handle - 1];
  Worlds()[handle - 1] = nullptr;
}

extern "C" uint64_t wolang_world_spawn(uint64_t handle) {
  World* world = Find(handle);
  if (world == nullptr) {
    return 0;
  }
  const uint64_t entity = world->next_entity++;
  world->alive[entity] = true;
  world->living.push_back(entity);
  return entity;
}

extern "C" bool wolang_world_alive(uint64_t handle, uint64_t entity) {
  World* world = Find(handle);
  if (world == nullptr) {
    return false;
  }
  const auto found = world->alive.find(entity);
  return found != world->alive.end() && found->second;
}

extern "C" uint64_t wolang_world_count(uint64_t handle) {
  World* world = Find(handle);
  if (world == nullptr) {
    return 0;
  }
  uint64_t living = 0;
  for (const auto& entry : world->alive) {
    living += entry.second ? 1 : 0;
  }
  return living;
}

extern "C" void* wolang_world_add(uint64_t handle,
                                  uint64_t entity,
                                  uint64_t token,
                                  uint64_t size) {
  World* world = Find(handle);
  if (world == nullptr || entity == 0 || size == 0) {
    return nullptr;
  }
  Column& column = world->columns[token];
  if (column.size == 0) {
    column.size = size;
  }
  const auto found = column.of_entity.find(entity);
  if (found != column.of_entity.end()) {
    return column.At(found->second);
  }
  const uint64_t index = column.Count();
  column.records.resize((index + 1) * column.size, 0);
  column.owners.push_back(entity);
  column.of_entity[entity] = index;
  std::memset(column.At(index), 0, static_cast<size_t>(column.size));
  return column.At(index);
}

extern "C" void* wolang_world_get(uint64_t handle,
                                  uint64_t entity,
                                  uint64_t token) {
  World* world = Find(handle);
  if (world == nullptr) {
    return nullptr;
  }
  const auto column = world->columns.find(token);
  if (column == world->columns.end()) {
    return nullptr;
  }
  const auto found = column->second.of_entity.find(entity);
  if (found == column->second.of_entity.end()) {
    return nullptr;
  }
  return column->second.At(found->second);
}

extern "C" bool wolang_world_has(uint64_t handle,
                                 uint64_t entity,
                                 uint64_t token) {
  return wolang_world_get(handle, entity, token) != nullptr;
}

extern "C" void wolang_world_remove(uint64_t handle,
                                    uint64_t entity,
                                    uint64_t token) {
  World* world = Find(handle);
  if (world == nullptr) {
    return;
  }
  const auto column = world->columns.find(token);
  if (column == world->columns.end()) {
    return;
  }
  Column& held = column->second;
  const auto found = held.of_entity.find(entity);
  if (found == held.of_entity.end()) {
    return;
  }
  // The last record moves into the hole, and its owner is told where it
  // went: that is what keeps the array dense without moving everything.
  const uint64_t hole = found->second;
  const uint64_t last = held.Count() - 1;
  if (hole != last) {
    std::memcpy(held.At(hole), held.At(last),
                static_cast<size_t>(held.size));
    held.owners[hole] = held.owners[last];
    held.of_entity[held.owners[hole]] = hole;
  }
  held.owners.pop_back();
  held.records.resize(static_cast<size_t>(held.Count() * held.size));
  held.of_entity.erase(entity);
}

extern "C" uint64_t wolang_world_component_count(uint64_t handle,
                                                 uint64_t token) {
  World* world = Find(handle);
  if (world == nullptr) {
    return 0;
  }
  const auto column = world->columns.find(token);
  return column == world->columns.end() ? 0 : column->second.Count();
}

extern "C" void wolang_world_despawn(uint64_t handle, uint64_t entity) {
  World* world = Find(handle);
  if (world == nullptr) {
    return;
  }
  for (auto& column : world->columns) {
    wolang_world_remove(handle, entity, column.first);
  }
  world->alive[entity] = false;
}

extern "C" uint64_t wolang_query_open(uint64_t handle,
                                      const uint64_t* required,
                                      uint64_t required_count,
                                      const uint64_t* excluded,
                                      uint64_t excluded_count) {
  World* world = Find(handle);
  if (world == nullptr) {
    return 0;
  }
  auto* cursor = new Cursor();
  cursor->world = world;

  // Every column the filter needs, resolved once. Asking the world for
  // each of them per entity meant a lock and two lookups per entity per
  // frame, which was most of what running a system cost (D101).
  std::vector<const Column*> also;
  for (uint64_t i = 1; i < required_count && required != nullptr; ++i) {
    const auto found = world->columns.find(required[i]);
    if (found == world->columns.end()) {
      // Nothing has it, so nothing matches.
      Cursors().push_back(cursor);
      return Cursors().size();
    }
    also.push_back(&found->second);
  }
  std::vector<const Column*> never;
  for (uint64_t i = 0; i < excluded_count && excluded != nullptr; ++i) {
    const auto found = world->columns.find(excluded[i]);
    if (found != world->columns.end()) {
      never.push_back(&found->second);
    }
  }

  // The walk goes along the first component the query named, because that
  // is the one its body binds -- not along the smallest, which would make
  // the bound component a lookup per entity.
  Column* first = nullptr;
  if (required_count != 0 && required != nullptr) {
    const auto found = world->columns.find(required[0]);
    first = found == world->columns.end() ? nullptr : &found->second;
  }

  if (required_count == 0) {
    for (const uint64_t entity : world->living) {
      const auto alive = world->alive.find(entity);
      if (alive != world->alive.end() && alive->second) {
        cursor->entities.push_back(entity);
      }
    }
  } else if (first != nullptr) {
    cursor->walked = first;
    const uint64_t count = first->Count();
    cursor->entities.reserve(static_cast<size_t>(count));
    cursor->records.reserve(static_cast<size_t>(count));
    for (uint64_t record = 0; record < count; ++record) {
      const uint64_t entity = first->owners[record];
      bool keep = true;
      for (const Column* column : also) {
        if (column->of_entity.find(entity) == column->of_entity.end()) {
          keep = false;
          break;
        }
      }
      for (const Column* column : never) {
        if (column->of_entity.find(entity) != column->of_entity.end()) {
          keep = false;
          break;
        }
      }
      if (keep) {
        cursor->entities.push_back(entity);
        cursor->records.push_back(record);
      }
    }
  }

  cursor->addresses.reserve(cursor->records.size());
  for (const uint64_t record : cursor->records) {
    cursor->addresses.push_back(cursor->walked == nullptr
                                    ? nullptr
                                    : cursor->walked->At(record));
  }

  // The columns this walk reads, resolved once.
  for (uint64_t i = 0; i < required_count && required != nullptr; ++i) {
    const auto found = world->columns.find(required[i]);
    cursor->held.push_back(Cursor::Held{
        required[i],
        found == world->columns.end() ? nullptr : &found->second, nullptr});
  }
  Cursors().push_back(cursor);
  return Cursors().size();
}

extern "C" bool wolang_query_next(uint64_t handle) {
  Cursor* cursor = FindCursor(handle);
  if (cursor == nullptr || cursor->at >= cursor->entities.size()) {
    return false;
  }
  ++cursor->at;
  return true;
}

extern "C" void* const* wolang_query_walk(uint64_t handle) {
  // Where every matching entity's walked component is, as one array: the
  // loop then needs no call at all, which is what makes a system over a
  // hundred thousand entities arithmetic rather than a hundred thousand
  // calls (D101).
  Cursor* cursor = FindCursor(handle);
  return cursor == nullptr || cursor->addresses.empty()
             ? nullptr
             : cursor->addresses.data();
}

extern "C" uint64_t wolang_query_length(uint64_t handle) {
  Cursor* cursor = FindCursor(handle);
  return cursor == nullptr ? 0 : cursor->addresses.size();
}

extern "C" void* wolang_query_step(uint64_t handle) {
  // One call per entity rather than two: the step and the address of what
  // the body walks are the same question, and a null answer is the end of
  // the walk -- the component is required, so no entity in the walk is
  // without it.
  Cursor* cursor = FindCursor(handle);
  if (cursor == nullptr || cursor->at >= cursor->entities.size()) {
    return nullptr;
  }
  ++cursor->at;
  return cursor->Current();
}

extern "C" uint64_t wolang_query_entity(uint64_t handle) {
  Cursor* cursor = FindCursor(handle);
  if (cursor == nullptr || cursor->at == 0 ||
      cursor->at > cursor->entities.size()) {
    return 0;
  }
  return cursor->entities[cursor->at - 1];
}

extern "C" void* wolang_query_component(uint64_t handle, uint64_t token) {
  Cursor* cursor = FindCursor(handle);
  if (cursor == nullptr) {
    return nullptr;
  }
  if (uint8_t* at = cursor->Locate(token); at != nullptr) {
    return at;
  }
  if (cursor->at == 0 || cursor->at > cursor->entities.size()) {
    return nullptr;
  }
  // A component the query did not name: looked up, because a body may
  // read one it did not declare -- whether that was allowed is the access
  // graph's business, not this one's.
  const uint64_t entity = cursor->entities[cursor->at - 1];
  const auto column = cursor->world->columns.find(token);
  if (column == cursor->world->columns.end()) {
    return nullptr;
  }
  const auto found = column->second.of_entity.find(entity);
  return found == column->second.of_entity.end()
             ? nullptr
             : column->second.At(found->second);
}

extern "C" void wolang_query_close(uint64_t handle) {
  if (handle == 0 || handle > Cursors().size()) {
    return;
  }
  delete Cursors()[handle - 1];
  Cursors()[handle - 1] = nullptr;
}

extern "C" void wolang_stage_add(void* system) {
  if (system != nullptr) {
    Stage().push_back(system);
  }
}

extern "C" void wolang_stage_run(uint64_t world, float delta) {
  using System = void (*)(uint64_t, float);
  std::vector<void*> stage;
  stage.swap(Stage());
  if (stage.empty()) {
    return;
  }
  // The last one runs on this thread: a stage of one costs no thread at
  // all, which is what a schedule with an ordering in it is mostly made
  // of.
  struct Work {
    System system;
    uint64_t world;
    float delta;
  };
  std::vector<uint64_t> started;
  std::vector<Work*> work;
  for (size_t i = 0; i + 1 < stage.size(); ++i) {
    auto* one =
        new Work{reinterpret_cast<System>(stage[i]), world, delta};
    work.push_back(one);
    const uint64_t handle = wolang_thread_spawn(
        [](void* raw) {
          auto* given = static_cast<Work*>(raw);
          given->system(given->world, given->delta);
        },
        one);
    if (handle == 0) {
      // No thread to be had: run it here rather than skipping it.
      one->system(world, delta);
      continue;
    }
    started.push_back(handle);
  }
  reinterpret_cast<System>(stage.back())(world, delta);
  for (const uint64_t handle : started) {
    wolang_thread_join(handle);
  }
  for (Work* one : work) {
    delete one;
  }
}

}  // namespace runtime
}  // namespace wolang
