// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_ENTROPY_H_
#define WOLANG_RUNTIME_ENTROPY_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

// Fills |into| with |count| bytes from the system's own source. Zero
// when it worked. What a token, a key or a salt is made of (D170).
int32_t wolang_entropy(uint8_t* into, uint64_t count);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_ENTROPY_H_
