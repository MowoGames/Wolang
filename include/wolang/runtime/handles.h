// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_HANDLES_H_
#define WOLANG_RUNTIME_HANDLES_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

// A file kept open, for a program that reads at offsets and appends --
// which is what a store does and what whole-file reading cannot express
// (D180).
int32_t wolang_file_open(const uint8_t* path, uint64_t length,
                         int32_t writable);
int64_t wolang_file_read_at(int32_t handle, int64_t offset,
                            uint8_t* into, uint64_t count);
int64_t wolang_file_append(int32_t handle, const uint8_t* bytes,
                           uint64_t count);
int64_t wolang_file_size(int32_t handle);
// What makes a write survive the machine losing power.
int32_t wolang_file_sync(int32_t handle);
int32_t wolang_file_truncate(int32_t handle, int64_t size);
int32_t wolang_file_close(int32_t handle);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_HANDLES_H_
