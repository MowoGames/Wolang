// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

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

// Storage for one object, which is what a `class` value points at. The
// same allocator a frame comes from: there is one heap, and a collector --
// when there is one -- will want to know about both.
extern "C" void* wolang_object_alloc(uint64_t size);
extern "C" void wolang_object_free(void* object);

// One more reference to |object|, and one fewer. `release` answers 1
// when that was the last one, which is the caller's cue to run the
// destructor and then free it -- a destructor is a method, and only the
// compiler knows how to call one (D193).
void wolang_object_retain(void* object);
int32_t wolang_object_release(void* object);

// Whether the task behind |handle| has finished.
bool wolang_sched_done(int32_t handle);

// Runs until the task behind |handle| has finished, and returns how many
// steps that took. Everything else ready runs too: a cooperative scheduler
// has nothing else to do while waiting.
int64_t wolang_sched_join(int32_t handle);

// How many tasks are not finished.
int32_t wolang_sched_pending();

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_SCHEDULER_H_
