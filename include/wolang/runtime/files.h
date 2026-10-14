// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_FILES_H_
#define WOLANG_RUNTIME_FILES_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Opening a file, with the platform's own numbers.
//
// `open` itself is a C call the language can make, but its flags are
// not the same on two systems: `O_CREAT` is 64 on Linux and 512 on
// Darwin. A library written in Wolang cannot say both, and guessing is
// how a file ends up opened read-only on one of them -- so the flags stay
// here, where the header that defines them is (D92).
extern "C" {

// Opens an existing file for reading. Negative on failure.
int32_t wolang_open_read(const char* path, uint64_t length);

// Opens a file for writing, making it or emptying it first.
int32_t wolang_open_write(const char* path, uint64_t length);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_FILES_H_
