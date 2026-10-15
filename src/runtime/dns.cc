// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/dns.h"

#if defined(_WIN32)
#include <winsock2.h>
// After winsock2, which has to come first or the older header wins.
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstddef>
#include <cstring>

namespace wolang {
namespace runtime {

extern "C" int32_t wolang_resolve(const uint8_t* host,
                                  uint64_t length,
                                  uint32_t* out) {
  // A name is not an address, and turning one into the other is the
  // system's job: `/etc/hosts`, the resolver's configuration, mDNS and
  // whatever else the machine has been told to use. A program that
  // implemented its own DNS would answer differently from everything
  // else on the machine (D148).
  if (host == nullptr || out == nullptr || length == 0) {
    return -1;
  }
#if defined(_WIN32)
  // Winsock has to be started before `getaddrinfo` answers, and a
  // program that resolved before it opened a socket would otherwise be
  // the one case that fails (D158).
  static const bool started = [] {
    WSADATA data;
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  if (!started) {
    return -1;
  }
#endif
  // The name arrives as a view with a length; `getaddrinfo` wants a C
  // string, so a copy with a zero after it is what crosses.
  char name[256] = {};
  if (length >= sizeof(name)) {
    return -1;
  }
  std::memcpy(name, host, static_cast<size_t>(length));

  addrinfo wanted = {};
  wanted.ai_family = AF_INET;
  wanted.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (::getaddrinfo(name, nullptr, &wanted, &found) != 0 ||
      found == nullptr) {
    return -1;
  }
  int32_t status = -1;
  for (const addrinfo* at = found; at != nullptr; at = at->ai_next) {
    if (at->ai_family != AF_INET || at->ai_addr == nullptr) {
      continue;
    }
    const auto* address =
        reinterpret_cast<const sockaddr_in*>(at->ai_addr);
    // In the order a person writes it: `127.0.0.1` is 0x7f000001, which
    // is what the socket floor builds its sixteen bytes from.
    *out = ntohl(address->sin_addr.s_addr);
    status = 0;
    break;
  }
  ::freeaddrinfo(found);
  return status;
}

}  // namespace runtime
}  // namespace wolang
