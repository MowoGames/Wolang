// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_ENV_H_
#define WOLANG_RUNTIME_ENV_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// The program's arguments and its environment.
//
// Arguments are *given* to the runtime rather than read from it: a binary
// gets them in `main`, and a program run by the driver gets whatever
// followed `--`, so the one place that knows them is the entry point
// (D115). Everything else asks here.
extern "C" {

void wolang_args_set(int32_t count, const char* const* values);
int32_t wolang_args_count(void);
// The |index|th argument and how long it is, or null.
const uint8_t* wolang_args_at(int32_t index, uint64_t* length);

// The value of the environment variable called |name|, or null.
const uint8_t* wolang_env_get(const uint8_t* name,
                              uint64_t name_length,
                              uint64_t* length);
// Sets one. Zero on success.
int32_t wolang_env_set(const uint8_t* name,
                       uint64_t name_length,
                       const uint8_t* value,
                       uint64_t value_length);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_ENV_H_
