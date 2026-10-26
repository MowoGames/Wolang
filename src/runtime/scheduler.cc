// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/scheduler.h"

#include "wolang/runtime/arena.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include <cstddef>
#include <deque>
#include <vector>

#include "wolang/runtime/builtins.h"

namespace wolang {
namespace runtime {
namespace {

struct TaskEntry {
  int32_t kind = 0;
  void* frame = nullptr;
  bool done = false;
};

struct SchedulerState {
  std::vector<TaskEntry> tasks;
  // Indices of the tasks that can make progress, oldest first.
  std::deque<int32_t> ready;
  ResumeFn resume = nullptr;
  void* context = nullptr;
};

SchedulerState& Current() {
  static SchedulerState state;
  return state;
}

}  // namespace

void InstallResume(ResumeFn resume, void* context) {
  SchedulerState& state = Current();
  state.resume = resume;
  state.context = context;
}

void ResetScheduler() {
  SchedulerState& state = Current();
  for (TaskEntry& task : state.tasks) {
    if (!task.done && task.frame != nullptr) {
      wolang_frame_free(task.frame);
    }
  }
  state.tasks.clear();
  state.ready.clear();
}

extern "C" {

int32_t wolang_sched_spawn(int32_t kind, void* frame) {
  SchedulerState& state = Current();
  const auto handle = static_cast<int32_t>(state.tasks.size());
  state.tasks.push_back(TaskEntry{kind, frame, false});
  state.ready.push_back(handle);
  return handle;
}

int64_t wolang_sched_run() {
  SchedulerState& state = Current();
  if (state.resume == nullptr) {
    return 0;
  }
  int64_t steps = 0;
  while (!state.ready.empty()) {
    const int32_t handle = state.ready.front();
    state.ready.pop_front();
    TaskEntry& task = state.tasks[static_cast<size_t>(handle)];
    if (task.done) {
      continue;
    }
    ++steps;
    const bool finished =
        state.resume(state.context, task.kind, task.frame);
    // Read again: resuming may have spawned, and the vector may have moved.
    TaskEntry& after = state.tasks[static_cast<size_t>(handle)];
    if (!finished) {
      // Back of the queue: everything else that can run goes first, which
      // is the whole of "cooperative and fair".
      state.ready.push_back(handle);
      continue;
    }
    after.done = true;
    if (after.frame != nullptr) {
      wolang_frame_free(after.frame);
      after.frame = nullptr;
    }
  }
  return steps;
}

// How many references an object has, kept in the word *in front of* it.
//
// In front rather than inside, so that no layout changes: a field is
// where it always was, and the three tiers go on agreeing about offsets
// without being told anything. The compiler only ever sees the address
// it was given (D193).
constexpr uint64_t kCountBytes = 16;

uint64_t* CountOf(void* object) {
  return reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(object) -
                                     kCountBytes);
}

// The second word in front: how many weak references there are, plus one
// for all the strong ones together. The storage goes when it reaches zero,
// which is after the object itself has (D207).
uint64_t* WeakCountOf(void* object) {
  return CountOf(object) + 1;
}

static std::atomic<int64_t> sm_LiveObjects{0};

static bool Traced() {
  static const bool sm_Traced = std::getenv("WOLANG_TRACE_RC") != nullptr;
  return sm_Traced;
}

int64_t wolang_objects_live() {
  return sm_LiveObjects.load(std::memory_order_relaxed);
}

void* wolang_object_alloc(uint64_t size) {
  // Zeroed, so that a field the constructor does not write reads as zero
  // rather than as whatever was there. The interpreter traps on reading an
  // uninitialized slot and the compiled tier zeroes one; an object is the
  // third place that has to agree.
  //
  // From the innermost `arena` block when there is one, which is how a
  // request handler pays for its allocations once.
  void* base = wolang_arena_alloc(size + kCountBytes);
  if (base == nullptr) {
    return nullptr;
  }
  auto* object = static_cast<uint8_t*>(base) + kCountBytes;
  if (!wolang_arena_owns(object)) {
    sm_LiveObjects.fetch_add(1, std::memory_order_relaxed);
    if (Traced()) {
      std::fprintf(stderr, "rc alloc %p %llu\n", static_cast<void*>(object), static_cast<unsigned long long>(size));
    }
  }
  // One reference: whoever asked for it. Sixteen bytes rather than eight
  // so that what comes back is still aligned for anything.
  *CountOf(object) = 1;
  *WeakCountOf(object) = 1;
  return object;
}

void wolang_object_retain(void* object) {
  if (object == nullptr || wolang_arena_owns(object)) {
    return;
  }
  const uint64_t before = std::atomic_ref<uint64_t>(*CountOf(object)).fetch_add(1, std::memory_order_relaxed);
  if (Traced()) {
    std::fprintf(stderr, "rc retain %p %llu\n", object, static_cast<unsigned long long>(before + 1));
  }
}

int32_t wolang_object_release(void* object) {
  // Whether this was the last reference, which is the caller's cue to
  // run the destructor and give the storage back. The decision is here
  // and the destructor is in the generated code, because a destructor is
  // a method and only the compiler knows how to call one.
  if (object == nullptr || wolang_arena_owns(object)) {
    return 0;
  }
  std::atomic_ref<uint64_t> count(*CountOf(object));
  if (count.load(std::memory_order_relaxed) == 0) {
    return 0;
  }
  const uint64_t before = count.fetch_sub(1, std::memory_order_acq_rel);
  if (Traced()) {
    std::fprintf(stderr, "rc release %p %llu\n", object, static_cast<unsigned long long>(before - 1));
  }
  return before == 1 ? 1 : 0;
}

void wolang_object_free(void* object) {
  // Storage an open `arena` block handed out is reclaimed by the block,
  // not by this: a destructor that runs inside one would otherwise free a
  // pointer the allocator never gave anyone (D113).
  if (object == nullptr || wolang_arena_owns(object)) {
    return;
  }
  sm_LiveObjects.fetch_sub(1, std::memory_order_relaxed);
  if (Traced()) {
    std::fprintf(stderr, "rc free %p 0\n", object);
  }
  wolang_object_weak_release(object);
}

void wolang_object_weak_retain(void* object) {
  if (object == nullptr || wolang_arena_owns(object)) {
    return;
  }
  std::atomic_ref<uint64_t>(*WeakCountOf(object)).fetch_add(1, std::memory_order_relaxed);
}

void wolang_object_weak_release(void* object) {
  if (object == nullptr || wolang_arena_owns(object)) {
    return;
  }
  if (std::atomic_ref<uint64_t>(*WeakCountOf(object)).fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::free(static_cast<uint8_t*>(object) - kCountBytes);
  }
}

int32_t wolang_object_upgrade(void* object) {
  if (object == nullptr) {
    return 0;
  }
  if (wolang_arena_owns(object)) {
    return 1;
  }

  std::atomic_ref<uint64_t> count(*CountOf(object));
  uint64_t seen = count.load(std::memory_order_relaxed);
  while (seen != 0) {
    if (count.compare_exchange_weak(seen, seen + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      return 1;
    }
  }
  return 0;
}

int64_t wolang_sched_join(int32_t handle) {
  SchedulerState& state = Current();
  if (state.resume == nullptr) {
    return 0;
  }
  int64_t steps = 0;
  while (!wolang_sched_done(handle) && !state.ready.empty()) {
    const int32_t next = state.ready.front();
    state.ready.pop_front();
    TaskEntry& task = state.tasks[static_cast<size_t>(next)];
    if (task.done) {
      continue;
    }
    ++steps;
    const bool finished = state.resume(state.context, task.kind,
                                       task.frame);
    TaskEntry& after = state.tasks[static_cast<size_t>(next)];
    if (!finished) {
      state.ready.push_back(next);
      continue;
    }
    after.done = true;
    if (after.frame != nullptr) {
      wolang_frame_free(after.frame);
      after.frame = nullptr;
    }
  }
  return steps;
}

bool wolang_sched_done(int32_t handle) {
  SchedulerState& state = Current();
  if (handle < 0 || static_cast<size_t>(handle) >= state.tasks.size()) {
    return true;
  }
  return state.tasks[static_cast<size_t>(handle)].done;
}

int32_t wolang_sched_pending() {
  SchedulerState& state = Current();
  int32_t pending = 0;
  for (const TaskEntry& task : state.tasks) {
    pending += task.done ? 0 : 1;
  }
  return pending;
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
