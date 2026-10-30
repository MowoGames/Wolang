// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Sockets, as one set of calls that mean the same thing everywhere.
//
// `std.net` used to bind the system's own `socket`, `send` and `close`.
// That works on every system that copied Berkeley's interface, and
// Windows is the one that did not quite: a socket there is a handle and
// not a descriptor, closing one is `closesocket`, the library has to be
// started before the first call, and `send` takes an `int` where every
// other system takes a `size_t`. A binding cannot be written twice in a
// language with no conditional compilation, so the difference lives here
// -- the same place `wolang_resolve` and `wolang_spawn` already do
// (D158).

#ifndef WOLANG_RUNTIME_SOCKETS_H_
#define WOLANG_RUNTIME_SOCKETS_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

// A TCP socket, or -1. |family| is 2 for IPv4.
int64_t wolang_socket_open(void);

// Connects to |address|, four bytes in the order they are written, on
// |port|. Zero when it worked.
int32_t wolang_socket_connect(int64_t socket, uint32_t address,
                              uint32_t port);

// Binds to |address| (zero for every interface) and listens.
int32_t wolang_socket_listen(int64_t socket, uint32_t address,
                             uint32_t port, int32_t backlog);

// The next connection, or -1 when there is none.
int64_t wolang_socket_accept(int64_t socket);
// The port a socket is bound to, which is how a listener bound to port 0
// says which one the system picked.
int32_t wolang_socket_port(int64_t socket);

// Sends and receives. Both answer how many bytes moved, zero when the
// other end closed, and -1 on an error.
int64_t wolang_socket_send(int64_t socket, const uint8_t* bytes,
                           int64_t count);
int64_t wolang_socket_receive(int64_t socket, uint8_t* bytes,
                              int64_t room);

int32_t wolang_socket_close(int64_t socket);
// Ends both directions, which wakes a thread blocked in `accept` or
// `receive` on it -- closing does not, everywhere.
int32_t wolang_socket_shutdown(int64_t socket);

// Whether a port may be reused the moment a program lets go of it, and
// whether small writes go out at once. The two options a server sets.
int32_t wolang_socket_reuse(int64_t socket, int32_t on);
int32_t wolang_socket_nodelay(int64_t socket, int32_t on);

// Whether a call that would wait answers instead.
int32_t wolang_socket_blocking(int64_t socket, int32_t on);

// UDP (D244). A datagram socket, or -1; bound with `wolang_udp_bind`
// (port 0 lets the system pick, `wolang_socket_port` says which).
// Closing, blocking and the port reuse the TCP calls above.
int64_t wolang_udp_open(void);
int32_t wolang_udp_bind(int64_t socket, uint32_t address, uint32_t port);
// How many bytes went, or -1 (also for an empty or over-65507-byte one).
int64_t wolang_udp_send_to(int64_t socket, uint32_t address, uint32_t port,
                           const uint8_t* bytes, int64_t count);
// One datagram into |bytes|, at most |room| of it. Answers the bytes
// stored, -2 when a non-blocking socket has nothing yet, -1 on an error.
// |truncated| is set when the datagram was longer than |room|.
int64_t wolang_udp_receive_from(int64_t socket, uint8_t* bytes,
                                int64_t room, uint32_t* address,
                                uint32_t* port, int32_t* truncated);
int32_t wolang_udp_broadcast(int64_t socket, int32_t on);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_SOCKETS_H_
