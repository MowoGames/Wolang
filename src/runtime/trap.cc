// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/trap.h"

#include <cstdio>
#include <cstdlib>

namespace wolang {
namespace runtime {
namespace {

// The names match the trap kinds in `ir/trap.h`. Spelled here rather than
// shared, because the runtime must not depend on the compiler: this file
// is linked into programs, not into the toolchain.
const char* KindName(int32_t kind) {
  switch (kind) {
    case 1:
      return "division by zero";
    case 2:
      return "arithmetic overflow";
    case 3:
      return "index out of bounds";
    case 4:
      return "undefined value";
    case 5:
      return "reached unreachable code";
    case 6:
      return "out of steps";
    case 7:
      return "no body to run";
    case 8:
      return "out of memory";
    default:
      return "trap";
  }
}

}  // namespace

extern "C" void wolang_trap(int32_t kind,
                            const char* message,
                            const char* function) {
  std::fprintf(stderr, "wolang: %s", KindName(kind));
  if (message != nullptr && *message != '\0') {
    std::fprintf(stderr, ": %s", message);
  }
  if (function != nullptr && *function != '\0') {
    std::fprintf(stderr, " in %s", function);
  }
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
  // Not `abort`: a trap is the program's own diagnosis, and a core dump
  // adds nothing to a message that already says which index and which
  // length.
  std::exit(70);
}

}  // namespace runtime
}  // namespace wolang
