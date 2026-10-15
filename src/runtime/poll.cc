// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/poll.h"

#include <fcntl.h>
#include <unistd.h>

#include <vector>

#if defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

namespace wolang {
namespace runtime {

extern "C" int32_t wolang_poll_create(void) {
#if defined(__APPLE__)
  return static_cast<int32_t>(::kqueue());
#else
  return static_cast<int32_t>(::epoll_create1(0));
#endif
}

extern "C" void wolang_poll_close(int32_t poller) {
  if (poller >= 0) {
    ::close(poller);
  }
}

extern "C" int32_t wolang_poll_add(int32_t poller, int32_t fd) {
#if defined(__APPLE__)
  struct kevent change;
  EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  return static_cast<int32_t>(
      ::kevent(poller, &change, 1, nullptr, 0, nullptr));
#else
  struct epoll_event event = {};
  event.events = EPOLLIN;
  event.data.fd = fd;
  return static_cast<int32_t>(
      ::epoll_ctl(poller, EPOLL_CTL_ADD, fd, &event));
#endif
}

extern "C" int32_t wolang_poll_remove(int32_t poller, int32_t fd) {
#if defined(__APPLE__)
  struct kevent change;
  EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  return static_cast<int32_t>(
      ::kevent(poller, &change, 1, nullptr, 0, nullptr));
#else
  return static_cast<int32_t>(
      ::epoll_ctl(poller, EPOLL_CTL_DEL, fd, nullptr));
#endif
}

extern "C" int32_t wolang_poll_wait(int32_t poller,
                                    int32_t timeout_ms,
                                    int32_t* out,
                                    int32_t max) {
  if (out == nullptr || max <= 0) {
    return -1;
  }
#if defined(__APPLE__)
  std::vector<struct kevent> events(static_cast<size_t>(max));
  struct timespec timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000;
  const int ready = ::kevent(poller, nullptr, 0, events.data(), max,
                             timeout_ms < 0 ? nullptr : &timeout);
  for (int i = 0; i < ready; ++i) {
    out[i] = static_cast<int32_t>(events[static_cast<size_t>(i)].ident);
  }
  return static_cast<int32_t>(ready);
#else
  std::vector<struct epoll_event> events(static_cast<size_t>(max));
  const int ready = ::epoll_wait(poller, events.data(), max, timeout_ms);
  for (int i = 0; i < ready; ++i) {
    out[i] = events[static_cast<size_t>(i)].data.fd;
  }
  return static_cast<int32_t>(ready);
#endif
}

extern "C" int32_t wolang_set_nonblocking(int32_t fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return static_cast<int32_t>(::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

}  // namespace runtime
}  // namespace wolang
