// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_POLL_H_
#define WOLANG_RUNTIME_POLL_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Which descriptors are ready, from whichever mechanism this system has.
//
// kqueue on Darwin and epoll on Linux: the same question asked twice in
// two vocabularies, and neither is something a library written in Wolang
// can spell -- the structures differ in size and field order (D95). So
// the three calls below are the floor, and the server above them is
// Wolang.
//
// One thread, many connections: that is the whole point. A server that
// blocks in `read` holds one connection at a time no matter how fast the
// rest of it is.
extern "C" {

// A fresh poller, or -1. Close it with `wolang_poll_close`.
int32_t wolang_poll_create(void);
void wolang_poll_close(int32_t poller);

// Watches |fd| for readability. Returns zero on success.
int32_t wolang_poll_add(int32_t poller, int32_t fd);
// Stops watching it, which happens by itself when the descriptor closes.
int32_t wolang_poll_remove(int32_t poller, int32_t fd);

// Waits up to |timeout_ms| for something to be readable and writes the
// ready descriptors into |out|, at most |max| of them. Returns how many,
// zero on timeout, or -1.
int32_t wolang_poll_wait(int32_t poller,
                         int32_t timeout_ms,
                         int32_t* out,
                         int32_t max);

// Makes |fd| non-blocking, so that a read which would wait returns
// instead. The flag is per descriptor and the number for it differs by
// system, which is why it is here.
int32_t wolang_set_nonblocking(int32_t fd);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_POLL_H_
