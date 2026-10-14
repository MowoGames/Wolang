// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/world.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

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
  uint8_t* At(uint64_t index) {
    return records.data() + index * size;
  }
};

struct World {
  uint64_t next_entity = 1;
  std::unordered_map<uint64_t, Column> columns;
  std::vector<uint64_t> living;
  std::unordered_map<uint64_t, bool> alive;
};

// A walk, with the entities it answered with already chosen.
struct Cursor {
  World* world = nullptr;
  std::vector<uint64_t> entities;
  // One past the entity the cursor is on; zero means "before the first".
  size_t at = 0;
};

std::mutex& Lock() {
  static std::mutex lock;
  return lock;
}

std::vector<World*>& Worlds() {
  static std::vector<World*> worlds;
  return worlds;
}

std::vector<Cursor*>& Cursors() {
  static std::vector<Cursor*> cursors;
  return cursors;
}

World* Find(uint64_t handle) {
  const std::lock_guard<std::mutex> held(Lock());
  if (handle == 0 || handle > Worlds().size()) {
    return nullptr;
  }
  return Worlds()[handle - 1];
}

Cursor* FindCursor(uint64_t handle) {
  const std::lock_guard<std::mutex> held(Lock());
  if (handle == 0 || handle > Cursors().size()) {
    return nullptr;
  }
  return Cursors()[handle - 1];
}

}  // namespace

extern "C" uint64_t wolang_world_create(void) {
  auto* world = new World();
  const std::lock_guard<std::mutex> held(Lock());
  Worlds().push_back(world);
  return Worlds().size();
}

extern "C" void wolang_world_destroy(uint64_t handle) {
  const std::lock_guard<std::mutex> held(Lock());
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

  // The smallest required column decides the walk: an entity that has
  // every one of them is in all of their columns, so the shortest is the
  // one worth walking.
  const Column* smallest = nullptr;
  for (uint64_t i = 0; i < required_count; ++i) {
    const auto found = world->columns.find(required[i]);
    if (found == world->columns.end()) {
      smallest = nullptr;
      break;
    }
    if (smallest == nullptr || found->second.Count() < smallest->Count()) {
      smallest = &found->second;
    }
  }

  if (required_count == 0) {
    for (const uint64_t entity : world->living) {
      if (wolang_world_alive(handle, entity)) {
        cursor->entities.push_back(entity);
      }
    }
  } else if (smallest != nullptr) {
    for (const uint64_t entity : smallest->owners) {
      bool keep = true;
      for (uint64_t i = 0; i < required_count && keep; ++i) {
        keep = wolang_world_has(handle, entity, required[i]);
      }
      for (uint64_t i = 0; i < excluded_count && keep; ++i) {
        keep = !wolang_world_has(handle, entity, excluded[i]);
      }
      if (keep) {
        cursor->entities.push_back(entity);
      }
    }
  }

  const std::lock_guard<std::mutex> held(Lock());
  Cursors().push_back(cursor);
  return Cursors().size();
}

extern "C" bool wolang_query_next(uint64_t handle) {
  Cursor* cursor = FindCursor(handle);
  if (cursor == nullptr) {
    return false;
  }
  if (cursor->at >= cursor->entities.size()) {
    return false;
  }
  ++cursor->at;
  return true;
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
  if (cursor == nullptr || cursor->at == 0 ||
      cursor->at > cursor->entities.size()) {
    return nullptr;
  }
  const uint64_t entity = cursor->entities[cursor->at - 1];
  const auto column = cursor->world->columns.find(token);
  if (column == cursor->world->columns.end()) {
    return nullptr;
  }
  const auto found = column->second.of_entity.find(entity);
  if (found == column->second.of_entity.end()) {
    return nullptr;
  }
  return column->second.At(found->second);
}

extern "C" void wolang_query_close(uint64_t handle) {
  const std::lock_guard<std::mutex> held(Lock());
  if (handle == 0 || handle > Cursors().size()) {
    return;
  }
  delete Cursors()[handle - 1];
  Cursors()[handle - 1] = nullptr;
}

}  // namespace runtime
}  // namespace wolang
