// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/host.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace wolang {
namespace runtime {

struct HostClass {
  uint64_t Tag = 0;
  HostRetain Retain = nullptr;
  HostRelease Release = nullptr;
  HostAlive Alive = nullptr;
  void* User = nullptr;
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
      entry = HostClass{tag, retain, release, alive, user};
      return;
    }
  }
  Classes().push_back(HostClass{tag, retain, release, alive, user});
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

}  // namespace runtime
}  // namespace wolang
