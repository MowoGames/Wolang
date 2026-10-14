// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/builtins.h"

#include "wolang/runtime/scheduler.h"

#include <cstdlib>
#include <cstring>

namespace wolang {
namespace runtime {

extern "C" {

void* wolang_frame_alloc(uint64_t size) {
  // Zeroed, because a frame that starts at state zero is a frame that
  // starts at the beginning -- and the driver should not have to write
  // every field it does not use.
  void* frame = std::calloc(1, size == 0 ? 1 : static_cast<size_t>(size));
  return frame;
}

void wolang_frame_free(void* frame) {
  std::free(frame);
}

}  // extern "C"

namespace {

struct Entry {
  std::string_view name;
  void* address;
};

}  // namespace

void* LookupSymbol(std::string_view name) {
  static const Entry kTable[] = {
      {"wolang_frame_alloc", reinterpret_cast<void*>(&wolang_frame_alloc)},
      {"wolang_frame_free", reinterpret_cast<void*>(&wolang_frame_free)},
      {"wolang_sched_spawn",
       reinterpret_cast<void*>(&wolang_sched_spawn)},
      {"wolang_sched_run", reinterpret_cast<void*>(&wolang_sched_run)},
      {"wolang_sched_done", reinterpret_cast<void*>(&wolang_sched_done)},
      {"wolang_sched_join", reinterpret_cast<void*>(&wolang_sched_join)},
      {"wolang_object_alloc",
       reinterpret_cast<void*>(&wolang_object_alloc)},
      {"wolang_object_free",
       reinterpret_cast<void*>(&wolang_object_free)},
      {"wolang_sched_pending",
       reinterpret_cast<void*>(&wolang_sched_pending)},
  };
  for (const Entry& entry : kTable) {
    if (entry.name == name) {
      return entry.address;
    }
  }
  return nullptr;
}

}  // namespace runtime
}  // namespace wolang
