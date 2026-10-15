// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_SPAWN_H_
#define WOLANG_RUNTIME_SPAWN_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

// Runs |command| with no shell, writes |input| to it, and collects what
// it said. The exit code, or a negative number when the system stopped
// it or it could not be started (D153).
int32_t wolang_spawn(const uint8_t* command,
                     uint64_t length,
                     const uint8_t* input,
                     uint64_t input_length,
                     uint8_t* out,
                     uint64_t out_room,
                     uint64_t* out_written,
                     uint8_t* errors,
                     uint64_t errors_room,
                     uint64_t* errors_written);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_SPAWN_H_
