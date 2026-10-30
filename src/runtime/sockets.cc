// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/sockets.h"

#if defined(_WIN32)
#include <winsock2.h>
// After winsock2, which has to come first or the older header wins.
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstring>

namespace wolang {
namespace runtime {
namespace {

#if defined(_WIN32)

// Winsock has to be started before anything else in it is called, and
// nothing says when to stop: a process that opened a socket keeps it
// until it exits, which is also when this would have been torn down.
bool Started() {
  static const bool started = [] {
    WSADATA data;
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return started;
}

using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;

int Close(Socket socket) { return ::closesocket(socket); }

#else

bool Started() { return true; }

using Socket = int;
constexpr Socket kNoSocket = -1;

int Close(Socket socket) { return ::close(socket); }

#endif

// The handle a Wolang `Socket` holds, as the system's own type.
Socket Held(int64_t socket) { return static_cast<Socket>(socket); }

sockaddr_in Where(uint32_t address, uint32_t port) {
  sockaddr_in where;
  std::memset(&where, 0, sizeof(where));
  where.sin_family = AF_INET;
  where.sin_port = htons(static_cast<uint16_t>(port));
  where.sin_addr.s_addr = htonl(address);
  return where;
}

}  // namespace

extern "C" int64_t wolang_socket_open(void) {
  if (!Started()) {
    return -1;
  }
  const Socket made = ::socket(AF_INET, SOCK_STREAM, 0);
  if (made == kNoSocket) {
    return -1;
  }
  return static_cast<int64_t>(made);
}

extern "C" int32_t wolang_socket_connect(int64_t socket, uint32_t address,
                                         uint32_t port) {
  const sockaddr_in where = Where(address, port);
  return ::connect(Held(socket),
                   reinterpret_cast<const sockaddr*>(&where),
                   sizeof(where)) == 0
             ? 0
             : -1;
}

extern "C" int32_t wolang_socket_listen(int64_t socket, uint32_t address,
                                        uint32_t port, int32_t backlog) {
  const sockaddr_in where = Where(address, port);
  if (::bind(Held(socket), reinterpret_cast<const sockaddr*>(&where),
             sizeof(where)) != 0) {
    return -1;
  }
  return ::listen(Held(socket), backlog) == 0 ? 0 : -1;
}

extern "C" int64_t wolang_socket_accept(int64_t socket) {
  const Socket taken = ::accept(Held(socket), nullptr, nullptr);
  if (taken == kNoSocket) {
    return -1;
  }
  return static_cast<int64_t>(taken);
}

extern "C" int32_t wolang_socket_shutdown(int64_t socket) {
#if defined(_WIN32)
  return ::shutdown(Held(socket), SD_BOTH) == 0 ? 0 : -1;
#else
  return ::shutdown(Held(socket), SHUT_RDWR) == 0 ? 0 : -1;
#endif
}

extern "C" int32_t wolang_socket_port(int64_t socket) {
  sockaddr_in where;
  std::memset(&where, 0, sizeof(where));
#if defined(_WIN32)
  int length = sizeof(where);
#else
  socklen_t length = sizeof(where);
#endif
  if (::getsockname(Held(socket), reinterpret_cast<sockaddr*>(&where), &length) != 0) {
    return -1;
  }
  return ntohs(where.sin_port);
}

extern "C" int64_t wolang_socket_send(int64_t socket,
                                      const uint8_t* bytes,
                                      int64_t count) {
  if (bytes == nullptr || count <= 0) {
    return 0;
  }
#if defined(_WIN32)
  const int written =
      ::send(Held(socket), reinterpret_cast<const char*>(bytes),
             static_cast<int>(count), 0);
  return written;
#else
  // `MSG_NOSIGNAL` where it exists: a write to a connection the other
  // end closed should answer an error, not kill the process.
#if defined(MSG_NOSIGNAL)
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  return ::send(Held(socket), bytes, static_cast<size_t>(count), flags);
#endif
}

extern "C" int64_t wolang_socket_receive(int64_t socket, uint8_t* bytes,
                                         int64_t room) {
  if (bytes == nullptr || room <= 0) {
    return 0;
  }
#if defined(_WIN32)
  return ::recv(Held(socket), reinterpret_cast<char*>(bytes),
                static_cast<int>(room), 0);
#else
  return ::recv(Held(socket), bytes, static_cast<size_t>(room), 0);
#endif
}

extern "C" int32_t wolang_socket_close(int64_t socket) {
  if (socket < 0) {
    return 0;
  }
  return Close(Held(socket)) == 0 ? 0 : -1;
}

extern "C" int32_t wolang_socket_reuse(int64_t socket, int32_t on) {
  const int value = on != 0 ? 1 : 0;
#if defined(_WIN32)
  return ::setsockopt(Held(socket), SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&value),
                      sizeof(value)) == 0
             ? 0
             : -1;
#else
  if (::setsockopt(Held(socket), SOL_SOCKET, SO_REUSEADDR, &value,
                   sizeof(value)) != 0) {
    return -1;
  }
#if defined(SO_REUSEPORT)
  // What lets several threads each have their own listener on one port,
  // which is how the server scales past one accept loop (D135).
  ::setsockopt(Held(socket), SOL_SOCKET, SO_REUSEPORT, &value,
               sizeof(value));
#endif
  return 0;
#endif
}

extern "C" int32_t wolang_socket_nodelay(int64_t socket, int32_t on) {
  const int value = on != 0 ? 1 : 0;
#if defined(_WIN32)
  return ::setsockopt(Held(socket), IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&value),
                      sizeof(value)) == 0
             ? 0
             : -1;
#else
  return ::setsockopt(Held(socket), IPPROTO_TCP, TCP_NODELAY, &value,
                      sizeof(value)) == 0
             ? 0
             : -1;
#endif
}

extern "C" int32_t wolang_socket_blocking(int64_t socket, int32_t on) {
#if defined(_WIN32)
  u_long waits = on != 0 ? 0 : 1;
  return ::ioctlsocket(Held(socket), FIONBIO, &waits) == 0 ? 0 : -1;
#else
  const int flags = ::fcntl(Held(socket), F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  const int wanted =
      on != 0 ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  return ::fcntl(Held(socket), F_SETFL, wanted) == 0 ? 0 : -1;
#endif
}

// ---- UDP (D244) ----------------------------------------------------------
// The largest payload an IPv4 datagram can carry (65535 less the IP and
// UDP headers). Anything bigger is refused here rather than left to each
// system to word its own error.
constexpr int64_t kMaxDatagram = 65507;

extern "C" int64_t wolang_udp_open(void) {
  if (!Started()) {
    return -1;
  }
  const Socket made = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (made == kNoSocket) {
    return -1;
  }
  return static_cast<int64_t>(made);
}

extern "C" int32_t wolang_udp_bind(int64_t socket, uint32_t address,
                                   uint32_t port) {
  const sockaddr_in where = Where(address, port);
  return ::bind(Held(socket), reinterpret_cast<const sockaddr*>(&where),
                sizeof(where)) == 0
             ? 0
             : -1;
}

extern "C" int64_t wolang_udp_send_to(int64_t socket, uint32_t address,
                                      uint32_t port, const uint8_t* bytes,
                                      int64_t count) {
  if (bytes == nullptr || count <= 0 || count > kMaxDatagram) {
    return -1;
  }
  const sockaddr_in where = Where(address, port);
#if defined(_WIN32)
  return ::sendto(Held(socket), reinterpret_cast<const char*>(bytes),
                  static_cast<int>(count), 0,
                  reinterpret_cast<const sockaddr*>(&where),
                  sizeof(where));
#else
  return ::sendto(Held(socket), bytes, static_cast<size_t>(count), 0,
                  reinterpret_cast<const sockaddr*>(&where),
                  sizeof(where));
#endif
}

extern "C" int64_t wolang_udp_receive_from(int64_t socket, uint8_t* bytes,
                                           int64_t room, uint32_t* address,
                                           uint32_t* port,
                                           int32_t* truncated) {
  if (bytes == nullptr || room <= 0 || address == nullptr ||
      port == nullptr || truncated == nullptr) {
    return -1;
  }
  if (room > kMaxDatagram) {
    room = kMaxDatagram;
  }
  *truncated = 0;
  sockaddr_in from;
  std::memset(&from, 0, sizeof(from));
#if defined(_WIN32)
  int length = sizeof(from);
  const int got =
      ::recvfrom(Held(socket), reinterpret_cast<char*>(bytes),
                 static_cast<int>(room), 0,
                 reinterpret_cast<sockaddr*>(&from), &length);
  if (got == SOCKET_ERROR) {
    const int why = ::WSAGetLastError();
    if (why == WSAEWOULDBLOCK) {
      return -2;
    }
    if (why != WSAEMSGSIZE) {
      return -1;
    }
    // The buffer is filled and the rest of the datagram is gone.
    *truncated = 1;
    *address = ntohl(from.sin_addr.s_addr);
    *port = ntohs(from.sin_port);
    return room;
  }
#else
  // `recvmsg` rather than `recvfrom` because only it reports `MSG_TRUNC`
  // the same way on Linux and the BSDs.
  iovec piece;
  piece.iov_base = bytes;
  piece.iov_len = static_cast<size_t>(room);
  msghdr message;
  std::memset(&message, 0, sizeof(message));
  message.msg_name = &from;
  message.msg_namelen = sizeof(from);
  message.msg_iov = &piece;
  message.msg_iovlen = 1;
  const ssize_t got = ::recvmsg(Held(socket), &message, 0);
  if (got < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1;
  }
  if ((message.msg_flags & MSG_TRUNC) != 0) {
    *truncated = 1;
  }
#endif
  *address = ntohl(from.sin_addr.s_addr);
  *port = ntohs(from.sin_port);
  return static_cast<int64_t>(got);
}

extern "C" int32_t wolang_udp_broadcast(int64_t socket, int32_t on) {
  const int value = on != 0 ? 1 : 0;
#if defined(_WIN32)
  return ::setsockopt(Held(socket), SOL_SOCKET, SO_BROADCAST,
                      reinterpret_cast<const char*>(&value),
                      sizeof(value)) == 0
             ? 0
             : -1;
#else
  return ::setsockopt(Held(socket), SOL_SOCKET, SO_BROADCAST, &value,
                      sizeof(value)) == 0
             ? 0
             : -1;
#endif
}

}  // namespace runtime
}  // namespace wolang
