// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_TASKS_H_
#define WOLANG_IR_TASKS_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "wolang/abi/layout.h"
#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
#include "wolang/ir/ir.h"

namespace wolang {
namespace ir {

// Splits every `task` function into a state machine, so that nothing below
// this point has to know what a coroutine is.
//
// A task is stackless: everything that outlives a suspension lives in a
// *frame*, and the function becomes `F$resume(frame) -> bool` with a switch
// over where it left off. A `wait` on another task puts that task's frame
// inside this one -- a static call graph needs no allocation per call -- and
// polls it: when the callee is not finished, the caller records its state
// and returns, which is what suspending is.
//
// After this pass there are no tasks, only functions and structs. That is
// the whole point of the stackless form: the interpreter and the code
// generator need to know nothing about suspension, and the differential
// check between them covers coroutines for free.
//
// `F` itself is kept as a driver that runs the machine to completion, so a
// direct call still works where no scheduler is running.
//
// Reports a task that waits on itself: its frame would contain itself.
// |span_of| turns a function's declaration -- the item index it carries --
// into a position, because the IR holds no source positions of its own
// (D31) and a diagnostic without one is a diagnostic nobody can act on.
void SplitTasks(StringInterner* interner,
                TypeStore* types,
                abi::LayoutStore* layouts,
                Module* module,
                const std::function<SourceSpan(uint32_t)>& span_of,
                std::vector<Diagnostic>* diagnostics);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_TASKS_H_
