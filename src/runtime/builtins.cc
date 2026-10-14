// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/builtins.h"

#include "wolang/runtime/arena.h"
#include "wolang/runtime/classes.h"
#include "wolang/runtime/files.h"
#include "wolang/runtime/globals.h"
#include "wolang/runtime/poll.h"
#include "wolang/runtime/threads.h"
#include "wolang/runtime/signal.h"
#include "wolang/runtime/text.h"

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
      {"wolang_open_read", reinterpret_cast<void*>(&wolang_open_read)},
      {"wolang_thread_spawn",
       reinterpret_cast<void*>(&wolang_thread_spawn)},
      {"wolang_thread_join",
       reinterpret_cast<void*>(&wolang_thread_join)},
      {"wolang_thread_count",
       reinterpret_cast<void*>(&wolang_thread_count)},
      {"wolang_mutex_create",
       reinterpret_cast<void*>(&wolang_mutex_create)},
      {"wolang_mutex_destroy",
       reinterpret_cast<void*>(&wolang_mutex_destroy)},
      {"wolang_mutex_lock",
       reinterpret_cast<void*>(&wolang_mutex_lock)},
      {"wolang_mutex_unlock",
       reinterpret_cast<void*>(&wolang_mutex_unlock)},
      {"wolang_atomic_add",
       reinterpret_cast<void*>(&wolang_atomic_add)},
      {"wolang_poll_create",
       reinterpret_cast<void*>(&wolang_poll_create)},
      {"wolang_poll_close", reinterpret_cast<void*>(&wolang_poll_close)},
      {"wolang_poll_add", reinterpret_cast<void*>(&wolang_poll_add)},
      {"wolang_poll_remove",
       reinterpret_cast<void*>(&wolang_poll_remove)},
      {"wolang_poll_wait", reinterpret_cast<void*>(&wolang_poll_wait)},
      {"wolang_set_nonblocking",
       reinterpret_cast<void*>(&wolang_set_nonblocking)},
      {"wolang_open_write", reinterpret_cast<void*>(&wolang_open_write)},
      {"wolang_class_register",
       reinterpret_cast<void*>(&wolang_class_register)},
      {"wolang_class_lookup",
       reinterpret_cast<void*>(&wolang_class_lookup)},
      {"wolang_global_address",
       reinterpret_cast<void*>(&wolang_global_address)},
      {"wolang_text_from_int",
       reinterpret_cast<void*>(&wolang_text_from_int)},
      {"wolang_text_from_float",
       reinterpret_cast<void*>(&wolang_text_from_float)},
      {"wolang_text_from_bool",
       reinterpret_cast<void*>(&wolang_text_from_bool)},
      {"wolang_text_concat",
       reinterpret_cast<void*>(&wolang_text_concat)},
      {"wolang_text_equal",
       reinterpret_cast<void*>(&wolang_text_equal)},
      {"wolang_arena_push",
       reinterpret_cast<void*>(&wolang_arena_push)},
      {"wolang_arena_pop", reinterpret_cast<void*>(&wolang_arena_pop)},
      {"wolang_arena_alloc",
       reinterpret_cast<void*>(&wolang_arena_alloc)},
      {"wolang_arena_depth",
       reinterpret_cast<void*>(&wolang_arena_depth)},
      {"wolang_arena_used", reinterpret_cast<void*>(&wolang_arena_used)},
      {"wolang_signal_connect",
       reinterpret_cast<void*>(&wolang_signal_connect)},
      {"wolang_signal_disconnect",
       reinterpret_cast<void*>(&wolang_signal_disconnect)},
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
