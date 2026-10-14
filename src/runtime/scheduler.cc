// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/scheduler.h"

#include <cstddef>
#include <deque>
#include <vector>

#include "wolang/runtime/builtins.h"

namespace wolang {
namespace runtime {
namespace {

struct Entry {
  int32_t kind = 0;
  void* frame = nullptr;
  bool done = false;
};

struct State {
  std::vector<Entry> tasks;
  // Indices of the tasks that can make progress, oldest first.
  std::deque<int32_t> ready;
  ResumeFn resume = nullptr;
  void* context = nullptr;
};

State& Current() {
  static State state;
  return state;
}

}  // namespace

void InstallResume(ResumeFn resume, void* context) {
  State& state = Current();
  state.resume = resume;
  state.context = context;
}

void ResetScheduler() {
  State& state = Current();
  for (Entry& task : state.tasks) {
    if (!task.done && task.frame != nullptr) {
      wolang_frame_free(task.frame);
    }
  }
  state.tasks.clear();
  state.ready.clear();
}

extern "C" {

int32_t wolang_sched_spawn(int32_t kind, void* frame) {
  State& state = Current();
  const auto handle = static_cast<int32_t>(state.tasks.size());
  state.tasks.push_back(Entry{kind, frame, false});
  state.ready.push_back(handle);
  return handle;
}

int64_t wolang_sched_run() {
  State& state = Current();
  if (state.resume == nullptr) {
    return 0;
  }
  int64_t steps = 0;
  while (!state.ready.empty()) {
    const int32_t handle = state.ready.front();
    state.ready.pop_front();
    Entry& task = state.tasks[static_cast<size_t>(handle)];
    if (task.done) {
      continue;
    }
    ++steps;
    const bool finished =
        state.resume(state.context, task.kind, task.frame);
    // Read again: resuming may have spawned, and the vector may have moved.
    Entry& after = state.tasks[static_cast<size_t>(handle)];
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

int64_t wolang_sched_join(int32_t handle) {
  State& state = Current();
  if (state.resume == nullptr) {
    return 0;
  }
  int64_t steps = 0;
  while (!wolang_sched_done(handle) && !state.ready.empty()) {
    const int32_t next = state.ready.front();
    state.ready.pop_front();
    Entry& task = state.tasks[static_cast<size_t>(next)];
    if (task.done) {
      continue;
    }
    ++steps;
    const bool finished = state.resume(state.context, task.kind,
                                       task.frame);
    Entry& after = state.tasks[static_cast<size_t>(next)];
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
  State& state = Current();
  if (handle < 0 || static_cast<size_t>(handle) >= state.tasks.size()) {
    return true;
  }
  return state.tasks[static_cast<size_t>(handle)].done;
}

int32_t wolang_sched_pending() {
  State& state = Current();
  int32_t pending = 0;
  for (const Entry& task : state.tasks) {
    pending += task.done ? 0 : 1;
  }
  return pending;
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
