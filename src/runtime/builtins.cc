// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/builtins.h"

#include "wolang/runtime/arena.h"
#include "wolang/runtime/classes.h"
#include "wolang/runtime/dns.h"
#include "wolang/runtime/entropy.h"
#include "wolang/runtime/zones.h"
#include "wolang/runtime/env.h"
#include "wolang/runtime/files.h"
#include "wolang/runtime/globals.h"
#include "wolang/runtime/handles.h"
#include "wolang/runtime/poll.h"
#include "wolang/runtime/threads.h"
#include "wolang/runtime/trap.h"
#include "wolang/runtime/world.h"
#include "wolang/runtime/signal.h"
#include "wolang/runtime/sockets.h"
#include "wolang/runtime/spawn.h"
#include "wolang/runtime/system.h"
#include "wolang/runtime/tls.h"
#include "wolang/runtime/text.h"

#include "wolang/runtime/scheduler.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

struct BuiltinEntry {
  std::string_view name;
  void* address;
};

}  // namespace

namespace {

// What a host registered, in order. A vector rather than a map: a host
// binds a handful of functions once, and a linear walk over ten strings
// is not worth a hash table's allocation.
std::vector<std::pair<std::string, void*>>& Registered() {
  static std::vector<std::pair<std::string, void*>> table;
  return table;
}

}  // namespace

void RegisterSymbol(std::string_view name, void* address) {
  for (auto& [existing, held] : Registered()) {
    if (existing == name) {
      held = address;
      return;
    }
  }
  Registered().emplace_back(std::string(name), address);
}

void ForgetSymbols() {
  Registered().clear();
}

void* LookupSymbol(std::string_view name) {
  // The host's own first: a program that binds `host_log` means its
  // `host_log` and not one that happens to be in the process.
  for (const auto& [registered, address] : Registered()) {
    if (registered == name) {
      return address;
    }
  }
  static const BuiltinEntry kTable[] = {
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
      {"wolang_objects_live", reinterpret_cast<void*>(&wolang_objects_live)},
      {"wolang_object_retain",
       reinterpret_cast<void*>(&wolang_object_retain)},
      {"wolang_object_release",
       reinterpret_cast<void*>(&wolang_object_release)},
      {"wolang_object_weak_retain", reinterpret_cast<void*>(&wolang_object_weak_retain)},
      {"wolang_object_weak_release", reinterpret_cast<void*>(&wolang_object_weak_release)},
      {"wolang_object_upgrade", reinterpret_cast<void*>(&wolang_object_upgrade)},
      {"wolang_open_read", reinterpret_cast<void*>(&wolang_open_read)},
      {"wolang_fs_kind", reinterpret_cast<void*>(&wolang_fs_kind)},
      {"wolang_fs_size", reinterpret_cast<void*>(&wolang_fs_size)},
      {"wolang_fs_changed", reinterpret_cast<void*>(&wolang_fs_changed)},
      {"wolang_fs_remove", reinterpret_cast<void*>(&wolang_fs_remove)},
      {"wolang_fs_rename", reinterpret_cast<void*>(&wolang_fs_rename)},
      {"wolang_fs_make_directory",
       reinterpret_cast<void*>(&wolang_fs_make_directory)},
      {"wolang_fs_list", reinterpret_cast<void*>(&wolang_fs_list)},
      {"wolang_fs_current",
       reinterpret_cast<void*>(&wolang_fs_current)},
      {"wolang_fs_set_current",
       reinterpret_cast<void*>(&wolang_fs_set_current)},
      {"wolang_fs_home", reinterpret_cast<void*>(&wolang_fs_home)},
      {"wolang_fs_temp_directory",
       reinterpret_cast<void*>(&wolang_fs_temp_directory)},
      {"wolang_fs_temp_name",
       reinterpret_cast<void*>(&wolang_fs_temp_name)},
      {"wolang_fs_absolute",
       reinterpret_cast<void*>(&wolang_fs_absolute)},
      {"wolang_fs_copy", reinterpret_cast<void*>(&wolang_fs_copy)},
      {"wolang_fs_remove_tree",
       reinterpret_cast<void*>(&wolang_fs_remove_tree)},
      {"wolang_fs_walk", reinterpret_cast<void*>(&wolang_fs_walk)},
      {"wolang_fs_append", reinterpret_cast<void*>(&wolang_fs_append)},
      {"wolang_file_open", reinterpret_cast<void*>(&wolang_file_open)},
      {"wolang_file_read_at",
       reinterpret_cast<void*>(&wolang_file_read_at)},
      {"wolang_file_append",
       reinterpret_cast<void*>(&wolang_file_append)},
      {"wolang_file_size", reinterpret_cast<void*>(&wolang_file_size)},
      {"wolang_file_sync", reinterpret_cast<void*>(&wolang_file_sync)},
      {"wolang_file_truncate",
       reinterpret_cast<void*>(&wolang_file_truncate)},
      {"wolang_file_close", reinterpret_cast<void*>(&wolang_file_close)},
      {"wolang_world_create",
       reinterpret_cast<void*>(&wolang_world_create)},
      {"wolang_world_destroy",
       reinterpret_cast<void*>(&wolang_world_destroy)},
      {"wolang_world_spawn",
       reinterpret_cast<void*>(&wolang_world_spawn)},
      {"wolang_world_despawn",
       reinterpret_cast<void*>(&wolang_world_despawn)},
      {"wolang_world_alive",
       reinterpret_cast<void*>(&wolang_world_alive)},
      {"wolang_world_count",
       reinterpret_cast<void*>(&wolang_world_count)},
      {"wolang_world_add",
       reinterpret_cast<void*>(&wolang_world_add)},
      {"wolang_world_get",
       reinterpret_cast<void*>(&wolang_world_get)},
      {"wolang_world_has",
       reinterpret_cast<void*>(&wolang_world_has)},
      {"wolang_world_remove",
       reinterpret_cast<void*>(&wolang_world_remove)},
      {"wolang_world_component_count",
       reinterpret_cast<void*>(&wolang_world_component_count)},
      {"wolang_query_open",
       reinterpret_cast<void*>(&wolang_query_open)},
      {"wolang_query_next",
       reinterpret_cast<void*>(&wolang_query_next)},
      {"wolang_query_step",
       reinterpret_cast<void*>(&wolang_query_step)},
      {"wolang_query_walk",
       reinterpret_cast<void*>(&wolang_query_walk)},
      {"wolang_query_length",
       reinterpret_cast<void*>(&wolang_query_length)},
      {"wolang_query_entity",
       reinterpret_cast<void*>(&wolang_query_entity)},
      {"wolang_query_component",
       reinterpret_cast<void*>(&wolang_query_component)},
      {"wolang_query_close",
       reinterpret_cast<void*>(&wolang_query_close)},
      {"wolang_stage_add", reinterpret_cast<void*>(&wolang_stage_add)},
      {"wolang_stage_run", reinterpret_cast<void*>(&wolang_stage_run)},
      {"wolang_args_count",
       reinterpret_cast<void*>(&wolang_args_count)},
      {"wolang_args_at", reinterpret_cast<void*>(&wolang_args_at)},
      {"wolang_resolve", reinterpret_cast<void*>(&wolang_resolve)},
      {"wolang_spawn", reinterpret_cast<void*>(&wolang_spawn)},
      {"wolang_spawn_in", reinterpret_cast<void*>(&wolang_spawn_in)},
      {"wolang_print", reinterpret_cast<void*>(&wolang_print)},
      {"wolang_text_compare",
       reinterpret_cast<void*>(&wolang_text_compare)},
      {"wolang_entropy", reinterpret_cast<void*>(&wolang_entropy)},
      {"wolang_zone_offset",
       reinterpret_cast<void*>(&wolang_zone_offset)},
      {"wolang_zone_name", reinterpret_cast<void*>(&wolang_zone_name)},
      {"wolang_socket_open",
       reinterpret_cast<void*>(&wolang_socket_open)},
      {"wolang_socket_connect",
       reinterpret_cast<void*>(&wolang_socket_connect)},
      {"wolang_socket_listen",
       reinterpret_cast<void*>(&wolang_socket_listen)},
      {"wolang_socket_accept",
       reinterpret_cast<void*>(&wolang_socket_accept)},
      {"wolang_socket_send",
       reinterpret_cast<void*>(&wolang_socket_send)},
      {"wolang_socket_receive",
       reinterpret_cast<void*>(&wolang_socket_receive)},
      {"wolang_socket_close",
       reinterpret_cast<void*>(&wolang_socket_close)},
      {"wolang_socket_reuse",
       reinterpret_cast<void*>(&wolang_socket_reuse)},
      {"wolang_socket_nodelay",
       reinterpret_cast<void*>(&wolang_socket_nodelay)},
      {"wolang_socket_blocking",
       reinterpret_cast<void*>(&wolang_socket_blocking)},
      {"wolang_env_get", reinterpret_cast<void*>(&wolang_env_get)},
      {"wolang_env_set", reinterpret_cast<void*>(&wolang_env_set)},
      {"wolang_report", reinterpret_cast<void*>(&wolang_report)},
      {"wolang_thread_spawn",
       reinterpret_cast<void*>(&wolang_thread_spawn)},
      {"wolang_thread_join",
       reinterpret_cast<void*>(&wolang_thread_join)},
      {"wolang_parallel_for",
       reinterpret_cast<void*>(&wolang_parallel_for)},
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
      {"wolang_text_free",
       reinterpret_cast<void*>(&wolang_text_free)},
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
      {"wolang_fd_read", reinterpret_cast<void*>(&wolang_fd_read)},
      {"wolang_fd_write", reinterpret_cast<void*>(&wolang_fd_write)},
      {"wolang_fd_close", reinterpret_cast<void*>(&wolang_fd_close)},
      {"wolang_process_id", reinterpret_cast<void*>(&wolang_process_id)},
      {"wolang_sleep_micros", reinterpret_cast<void*>(&wolang_sleep_micros)},
      {"wolang_clock", reinterpret_cast<void*>(&wolang_clock)},
      {"wolang_system", reinterpret_cast<void*>(&wolang_system)},
      {"wolang_tls_open", reinterpret_cast<void*>(&wolang_tls_open)},
      {"wolang_tls_write", reinterpret_cast<void*>(&wolang_tls_write)},
      {"wolang_tls_read", reinterpret_cast<void*>(&wolang_tls_read)},
      {"wolang_tls_close", reinterpret_cast<void*>(&wolang_tls_close)},
  };
  for (const BuiltinEntry& entry : kTable) {
    if (entry.name == name) {
      return entry.address;
    }
  }
  return nullptr;
}

}  // namespace runtime
}  // namespace wolang
