// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/sockets.h"

#include <cstdint>

#include "harness/test.h"

namespace wolang {
namespace runtime {
namespace {

constexpr uint32_t kLoopback = 0x7F000001u;

// Never writes past |room|, and says so when the datagram was longer (D244).
WOLANG_TEST(Udp, TruncatesToTheBufferAndReportsIt) {
  const int64_t a = wolang_udp_open();
  const int64_t b = wolang_udp_open();
  WOLANG_ASSERT(a >= 0 && b >= 0);
  WOLANG_ASSERT(wolang_udp_bind(a, kLoopback, 0) == 0);
  WOLANG_ASSERT(wolang_udp_bind(b, kLoopback, 0) == 0);
  const uint32_t to = static_cast<uint32_t>(wolang_socket_port(b));
  const uint8_t message[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  WOLANG_EXPECT(wolang_udp_send_to(a, kLoopback, to, message, 8) == 8);
  uint8_t room[5] = {0, 0, 0, 0, 0xEE};
  uint32_t address = 0;
  uint32_t port = 0;
  int32_t truncated = 0;
  WOLANG_EXPECT(wolang_udp_receive_from(b, room, 4, &address, &port,
                                        &truncated) == 4);
  WOLANG_EXPECT(truncated == 1);
  WOLANG_EXPECT(room[3] == 4 && room[4] == 0xEE);
  WOLANG_EXPECT(address == kLoopback);
  WOLANG_EXPECT(port == static_cast<uint32_t>(wolang_socket_port(a)));
  wolang_socket_close(a);
  wolang_socket_close(b);
}

WOLANG_TEST(Udp, EmptyNonBlockingReceiveIsMinusTwo) {
  const int64_t s = wolang_udp_open();
  WOLANG_ASSERT(s >= 0);
  WOLANG_ASSERT(wolang_udp_bind(s, kLoopback, 0) == 0);
  WOLANG_ASSERT(wolang_socket_blocking(s, 0) == 0);
  uint8_t room[8];
  uint32_t address = 0;
  uint32_t port = 0;
  int32_t truncated = 0;
  WOLANG_EXPECT(wolang_udp_receive_from(s, room, 8, &address, &port,
                                        &truncated) == -2);
  wolang_socket_close(s);
}

WOLANG_TEST(Udp, RefusesOversizedAndEmptySends) {
  const int64_t s = wolang_udp_open();
  WOLANG_ASSERT(s >= 0);
  static uint8_t big[65508];
  WOLANG_EXPECT(wolang_udp_send_to(s, kLoopback, 9, big, 65508) == -1);
  WOLANG_EXPECT(wolang_udp_send_to(s, kLoopback, 9, big, 0) == -1);
  wolang_socket_close(s);
}

}  // namespace
}  // namespace runtime
}  // namespace wolang
