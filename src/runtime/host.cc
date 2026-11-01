// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/host.h"

#include <cstdint>
#include <mutex>
#include <new>
#include <vector>

#include "wolang/runtime/globals.h"

namespace wolang {
namespace runtime {

struct HostClass {
  uint64_t Tag = 0;
  HostRetain Retain = nullptr;
  HostRelease Release = nullptr;
  HostAlive Alive = nullptr;
  void* User = nullptr;
  HostToken Token = nullptr;
};

struct WeakCell {
  void* Object = nullptr;
  uint64_t Token = 0;
};

static std::mutex& Lock() {
  static std::mutex sm_Lock;
  return sm_Lock;
}

static std::vector<HostClass>& Classes() {
  static std::vector<HostClass> sm_Classes;
  return sm_Classes;
}

static HostClass FindHostClass(uint64_t tag) {
  SpaceClass owned;
  if (SpaceClassOf(tag, &owned)) {
    return HostClass{owned.Tag, owned.Retain, owned.Release, owned.Alive, owned.User, owned.Token};
  }
  const std::lock_guard<std::mutex> held(Lock());
  for (const HostClass& entry : Classes()) {
    if (entry.Tag == tag) {
      return entry;
    }
  }
  return HostClass();
}

extern "C" void wolang_host_register(uint64_t tag, HostRetain retain, HostRelease release, HostAlive alive, void* user) {
  const std::lock_guard<std::mutex> held(Lock());
  for (HostClass& entry : Classes()) {
    if (entry.Tag == tag) {
      const HostToken token = entry.Token;
      entry = HostClass{tag, retain, release, alive, user, token};
      return;
    }
  }
  Classes().push_back(HostClass{tag, retain, release, alive, user, nullptr});
}

extern "C" void wolang_host_register_token(uint64_t tag, HostToken token) {
  const std::lock_guard<std::mutex> held(Lock());
  for (HostClass& entry : Classes()) {
    if (entry.Tag == tag) {
      entry.Token = token;
      return;
    }
  }
  HostClass entry;
  entry.Tag = tag;
  entry.Token = token;
  Classes().push_back(entry);
}

extern "C" void wolang_host_retain(uint64_t tag, void* object) {
  if (object == nullptr) {
    return;
  }
  const HostClass entry = FindHostClass(tag);
  if (entry.Retain != nullptr) {
    entry.Retain(object, entry.User);
  }
}

extern "C" void wolang_host_release(uint64_t tag, void* object) {
  if (object == nullptr) {
    return;
  }
  const HostClass entry = FindHostClass(tag);
  if (entry.Release != nullptr) {
    entry.Release(object, entry.User);
  }
}

extern "C" int32_t wolang_host_upgrade(uint64_t tag, void* object) {
  if (object == nullptr) {
    return 0;
  }
  const HostClass entry = FindHostClass(tag);
  if (entry.Alive != nullptr && entry.Alive(object, entry.User) == 0) {
    return 0;
  }
  if (entry.Retain != nullptr) {
    entry.Retain(object, entry.User);
  }
  return 1;
}

extern "C" uint64_t wolang_host_weak_make(uint64_t tag, uint64_t object) {
  if (object == 0) {
    return 0;
  }
  const HostClass entry = FindHostClass(tag);
  if (entry.Token == nullptr) {
    return object;
  }
  WeakCell* cell = new (std::nothrow) WeakCell();
  if (cell == nullptr) {
    return 0;
  }
  cell->Object = reinterpret_cast<void*>(object);
  cell->Token = entry.Token(cell->Object, entry.User);
  return reinterpret_cast<uint64_t>(cell);
}

extern "C" uint64_t wolang_host_weak_upgrade(uint64_t tag, uint64_t slot) {
  if (slot == 0) {
    return 0;
  }
  const HostClass entry = FindHostClass(tag);
  void* object = reinterpret_cast<void*>(slot);
  const WeakCell* cell = entry.Token != nullptr ? reinterpret_cast<const WeakCell*>(slot) : nullptr;
  if (cell != nullptr) {
    object = cell->Object;
  }
  if (entry.Alive != nullptr && entry.Alive(object, entry.User) == 0) {
    return 0;
  }
  if (cell != nullptr && entry.Token(object, entry.User) != cell->Token) {
    return 0;
  }
  if (entry.Retain != nullptr) {
    entry.Retain(object, entry.User);
  }
  return reinterpret_cast<uint64_t>(object);
}

extern "C" void wolang_host_weak_free(uint64_t tag, uint64_t slot) {
  if (slot == 0) {
    return;
  }
  if (FindHostClass(tag).Token == nullptr) {
    return;
  }
  delete reinterpret_cast<WeakCell*>(slot);
}

}  // namespace runtime
}  // namespace wolang
