// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// What the interpreter tells a debugger, and what it asks one (D216).

#ifndef WOLANG_INTERP_DEBUG_H_
#define WOLANG_INTERP_DEBUG_H_

#include <cstddef>
#include <string>
#include <vector>

#include "wolang/base/source_manager.h"

namespace wolang {
namespace interp {

// One name a paused program can see, as a debugger shows it: what it is
// called, what it holds, of what type -- and for an object or a struct,
// its fields, a level at a time.
struct DebugVariable {
  std::string Name;
  std::string Value;
  std::string Type;
  std::vector<DebugVariable> Fields;
};

// One call on the stack: the function, where in the source it is, and
// what it can see.
struct DebugFrame {
  std::string Function;
  SourceSpan Span;
  std::vector<DebugVariable> Variables;
};

class Debugger {
 public:
  virtual ~Debugger() = default;

  // Asked when the program reaches a new place in the source, |depth|
  // calls deep. Cheap on purpose: it is asked all the time, and answers
  // whether to stop here.
  virtual bool Wants(SourceSpan span, size_t depth) = 0;

  // The program is stopped, |stack| innermost first. Returns when it
  // should go on; false ends the run.
  virtual bool Paused(const std::vector<DebugFrame>& stack) = 0;
};

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_DEBUG_H_
