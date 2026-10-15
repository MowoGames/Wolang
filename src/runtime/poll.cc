// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/poll.h"

#include <vector>

#if defined(_WIN32)
// `WSAPoll` is this system's `poll` under another name: there is no
// readiness *queue* to add to, so the poller here is a list of sockets
// kept on the side and asked about on each wait. Which is what `select`
// always was, and is enough at the scale a poller is reached for before
// the one that scales is written (D159).
#include <winsock2.h>
#else
#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif
#endif

namespace wolang {
namespace runtime {

#if defined(_WIN32)
namespace {

// One list of sockets per poller, and a small number of pollers: a
// server opens one.
std::vector<std::vector<int32_t>>& Pollers() {
  static std::vector<std::vector<int32_t>> pollers;
  return pollers;
}

}  // namespace

extern "C" int32_t wolang_poll_create(void) {
  Pollers().emplace_back();
  return static_cast<int32_t>(Pollers().size() - 1);
}

extern "C" void wolang_poll_close(int32_t poller) {
  if (poller >= 0 &&
      static_cast<size_t>(poller) < Pollers().size()) {
    Pollers()[static_cast<size_t>(poller)].clear();
  }
}

extern "C" int32_t wolang_poll_add(int32_t poller, int32_t fd) {
  if (poller < 0 || static_cast<size_t>(poller) >= Pollers().size()) {
    return -1;
  }
  Pollers()[static_cast<size_t>(poller)].push_back(fd);
  return 0;
}

extern "C" int32_t wolang_poll_remove(int32_t poller, int32_t fd) {
  if (poller < 0 || static_cast<size_t>(poller) >= Pollers().size()) {
    return -1;
  }
  std::vector<int32_t>& held = Pollers()[static_cast<size_t>(poller)];
  for (size_t i = 0; i < held.size(); ++i) {
    if (held[i] == fd) {
      held.erase(held.begin() + static_cast<long>(i));
      return 0;
    }
  }
  return -1;
}

extern "C" int32_t wolang_poll_wait(int32_t poller, int32_t timeout_ms,
                                    int32_t* out, int32_t max) {
  if (out == nullptr || max <= 0 || poller < 0 ||
      static_cast<size_t>(poller) >= Pollers().size()) {
    return -1;
  }
  const std::vector<int32_t>& held =
      Pollers()[static_cast<size_t>(poller)];
  if (held.empty()) {
    return 0;
  }
  std::vector<WSAPOLLFD> asking(held.size());
  for (size_t i = 0; i < held.size(); ++i) {
    asking[i].fd = static_cast<SOCKET>(held[i]);
    asking[i].events = POLLRDNORM;
    asking[i].revents = 0;
  }
  const int ready = ::WSAPoll(asking.data(),
                              static_cast<ULONG>(asking.size()),
                              timeout_ms);
  if (ready <= 0) {
    return ready;
  }
  int32_t written = 0;
  for (size_t i = 0; i < asking.size() && written < max; ++i) {
    if (asking[i].revents != 0) {
      out[written++] = held[i];
    }
  }
  return written;
}

extern "C" int32_t wolang_set_nonblocking(int32_t fd) {
  u_long on = 1;
  return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &on) == 0 ? 0
                                                                   : -1;
}

#else

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

#endif  // the system

}  // namespace runtime
}  // namespace wolang
