// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_SYSTEM_H_
#define WOLANG_RUNTIME_SYSTEM_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

int64_t wolang_fd_read(int32_t fd, uint8_t* buffer, uint64_t count);
int64_t wolang_fd_write(int32_t fd, const uint8_t* buffer, uint64_t count);
int32_t wolang_fd_close(int32_t fd);
int32_t wolang_process_id();
void wolang_sleep_micros(uint64_t micros);
int32_t wolang_clock(int32_t clock, int64_t* out);
int32_t wolang_system();

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_SYSTEM_H_
