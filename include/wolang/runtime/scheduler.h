// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_SCHEDULER_H_
#define WOLANG_RUNTIME_SCHEDULER_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// What runs the tasks.
//
// The scheduler holds pairs of "which machine" and "which frame" and calls
// the one entry point the compiler generates for every task, so it needs no
// function pointers into generated code and works the same whether the
// program is being interpreted or has been compiled. That is also why it can
// live here, in one place, rather than once per tier.
//
// Round robin, cooperative, single threaded: a task runs until it suspends,
// and then the next one does. Parallelism is a later wave; interleaving is
// what makes a hundred thousand waiting tasks cost a frame each instead of a
// stack each.

// How a task is advanced by one step: the kind the compiler assigned it and
// the frame it keeps its state in. Returns true when the task is finished.
using ResumeFn = bool (*)(void* context, int32_t kind, void* frame);

// Installs what the scheduler resumes through. The driver points this at
// whichever tier is running the program.
void InstallResume(ResumeFn resume, void* context);

// Forgets every task. Called between runs so one program's tasks cannot
// outlive it.
void ResetScheduler();

extern "C" {

// Hands a task to the scheduler and returns a handle for it.
int32_t wolang_sched_spawn(int32_t kind, void* frame);

// Runs until nothing is left to run. Returns how many steps it took, which
// is what a benchmark wants to see.
int64_t wolang_sched_run();

// Whether the task behind |handle| has finished.
bool wolang_sched_done(int32_t handle);

// How many tasks are not finished.
int32_t wolang_sched_pending();

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_SCHEDULER_H_
