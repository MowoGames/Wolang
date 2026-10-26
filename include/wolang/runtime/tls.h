// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_TLS_H_
#define WOLANG_RUNTIME_TLS_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

// A TLS client over a connected socket, through the system's own TLS:
// Schannel on Windows, OpenSSL loaded at run time elsewhere (D211).
// Zero when the handshake or the certificate check failed.
int64_t wolang_tls_open(int64_t socket, const uint8_t* host, uint64_t length, int32_t verify);
int64_t wolang_tls_write(int64_t session, const uint8_t* bytes, int64_t count);
int64_t wolang_tls_read(int64_t session, uint8_t* bytes, int64_t room);
void wolang_tls_close(int64_t session);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_TLS_H_
