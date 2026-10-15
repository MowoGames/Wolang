// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Names into addresses, through the system's own resolver.

#ifndef WOLANG_RUNTIME_DNS_H_
#define WOLANG_RUNTIME_DNS_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

// Resolves |host| to an IPv4 address, in host order. Zero on success.
//
// The system's resolver, because that is what every other program on the
// machine uses: a language with its own DNS would answer differently
// from the machine it runs on (D148).
int32_t wolang_resolve(const uint8_t* host, uint64_t length, uint32_t* out);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_DNS_H_
