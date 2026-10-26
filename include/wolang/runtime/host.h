// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_HOST_H_
#define WOLANG_RUNTIME_HOST_H_

#include <cstdint>
#include <string_view>

namespace wolang {
namespace runtime {

// Which `extern class` an object is, as the compiler and the host both
// spell it: the name, hashed (D213).
inline uint64_t HostTag(std::string_view name) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : name) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

using HostRetain = void (*)(void* object, void* user);
using HostRelease = void (*)(void* object, void* user);
using HostAlive = int32_t (*)(void* object, void* user);

extern "C" {

// What the host does when a script takes or lets go of one of its objects,
// and how it says that an object a weak reference points at is gone.
void wolang_host_register(uint64_t tag, HostRetain retain, HostRelease release, HostAlive alive, void* user);
void wolang_host_retain(uint64_t tag, void* object);
void wolang_host_release(uint64_t tag, void* object);
// 1 and a reference taken when the object is still there, 0 when it is not.
int32_t wolang_host_upgrade(uint64_t tag, void* object);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_HOST_H_
