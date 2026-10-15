// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Randomness that is not a sequence.
//
// `std.random` is a generator: fast, repeatable, and exactly wrong for a
// token or a key. This is the system's own -- `getentropy` where there
// is one, the cryptographic provider on Windows -- because a language
// that rolled its own would be rolling its own mistakes (D170).

#include "wolang/runtime/entropy.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <bcrypt.h>
#else
// `getentropy` lives here on both systems that have it; `unistd.h` is
// where the older documentation says it is and where it is not.
#include <sys/random.h>
#include <unistd.h>

#include <cstdio>
#endif

#include <cstring>

namespace wolang {
namespace runtime {

extern "C" int32_t wolang_entropy(uint8_t* into, uint64_t count) {
  if (into == nullptr || count == 0) {
    return -1;
  }
#if defined(_WIN32)
  const NTSTATUS held = ::BCryptGenRandom(
      nullptr, into, static_cast<ULONG>(count),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return held == 0 ? 0 : -1;
#else
  uint64_t at = 0;
  while (at < count) {
    // `getentropy` takes at most 256 bytes at a time, which is the one
    // thing about it a caller has to know.
    const uint64_t piece = count - at > 256 ? 256 : count - at;
    if (::getentropy(into + at, static_cast<size_t>(piece)) != 0) {
      // A system that has none: the device, which every Unix has.
      std::FILE* device = std::fopen("/dev/urandom", "rb");
      if (device == nullptr) {
        return -1;
      }
      const size_t read = std::fread(into + at, 1,
                                     static_cast<size_t>(piece), device);
      std::fclose(device);
      if (read != piece) {
        return -1;
      }
    }
    at += piece;
  }
  return 0;
#endif
}

}  // namespace runtime
}  // namespace wolang
