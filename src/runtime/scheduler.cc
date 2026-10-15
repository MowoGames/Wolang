// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/scheduler.h"

#include "wolang/runtime/arena.h"

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

void* wolang_object_alloc(uint64_t size) {
  // Zeroed, so that a field the constructor does not write reads as zero
  // rather than as whatever was there. The interpreter traps on reading an
  // uninitialized slot and the compiled tier zeroes one; an object is the
  // third place that has to agree.
  //
  // From the innermost `arena` block when there is one, which is how a
  // request handler pays for its allocations once.
  return wolang_arena_alloc(size);
}

void wolang_object_free(void* object) {
  // Storage an open `arena` block handed out is reclaimed by the block,
  // not by this: a destructor that runs inside one would otherwise free a
  // pointer the allocator never gave anyone (D113).
  if (wolang_arena_owns(object)) {
    return;
  }
  std::free(object);
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
