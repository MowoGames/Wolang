// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/trap.h"

#include <cstdio>
#include <cstdlib>

namespace wolang {
namespace runtime {
namespace {

// The names match the trap kinds in `ir/trap.h`. Spelled here rather than
// shared, because the runtime must not depend on the compiler: this file
// is linked into programs, not into the toolchain.
// The same words the compiler's `TrapKindName` uses, so that a trap in a
// binary and one reported by a tier with a host under it read the same --
// which is what lets the differential fuzzer compare them (D98).
const char* KindName(int32_t kind) {
  switch (kind) {
    case 1:
      return "division by zero";
    case 2:
      return "overflow";
    case 3:
      return "out of bounds";
    case 4:
      return "undefined value";
    case 5:
      return "unreachable";
    case 6:
      return "step limit";
    case 7:
      return "no body";
    case 8:
      return "out of memory";
    default:
      return "trap";
  }
}

}  // namespace

extern "C" void wolang_report(int64_t value) {
  // What a program built for the differential fuzzer says it computed.
  std::fprintf(stdout, "value %lld\n", static_cast<long long>(value));
  std::fflush(stdout);
}

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
