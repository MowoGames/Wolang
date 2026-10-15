// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_TRAP_H_
#define WOLANG_RUNTIME_TRAP_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// What a trap does in a binary with no interpreter under it.
//
// The dev tier unwinds into the host and reports a trap as an outcome,
// because the host is right there. A shipping binary has nobody to
// report to, so it says what happened on standard error and stops --
// which is what an index out of bounds has to do if it is not to be
// undefined behaviour (D98).
extern "C" {

[[noreturn]] void wolang_trap(int32_t kind,
                              const char* message,
                              const char* function);

// What a program built by `wolang fuzz --aot` says it computed. Its own
// entry point rather than `Print`, so that a generated program needs no
// library and the driver has one line to read.
void wolang_report(int64_t value);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_TRAP_H_
