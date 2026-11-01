// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/system.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <io.h>
#include <process.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdint>

namespace wolang {
namespace runtime {

static constexpr int32_t kRealtime = 0;

static std::atomic<WolangFdWriteHook> sm_FdWriteHook{nullptr};
static std::atomic<void*> sm_FdWriteUser{nullptr};

extern "C" void wolang_set_fd_write_hook(WolangFdWriteHook hook, void* user) {
  sm_FdWriteUser.store(user);
  sm_FdWriteHook.store(hook);
}

extern "C" int64_t wolang_fd_read(int32_t fd, uint8_t* buffer, uint64_t count) {
#if defined(_WIN32)
  const uint32_t clamped = count > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<uint32_t>(count);
  return _read(fd, buffer, clamped);
#else
  return ::read(fd, buffer, count);
#endif
}

extern "C" int64_t wolang_fd_write(int32_t fd, const uint8_t* buffer, uint64_t count) {
  if (const WolangFdWriteHook hook = sm_FdWriteHook.load(); hook != nullptr && (fd == 1 || fd == 2)) {
    const int64_t handled = hook(sm_FdWriteUser.load(), fd, buffer, count);
    if (handled >= 0) {
      return handled;
    }
  }

#if defined(_WIN32)
  const uint32_t clamped = count > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<uint32_t>(count);
  return _write(fd, buffer, clamped);
#else
  return ::write(fd, buffer, count);
#endif
}

extern "C" int32_t wolang_fd_close(int32_t fd) {
#if defined(_WIN32)
  return _close(fd);
#else
  return ::close(fd);
#endif
}

extern "C" int32_t wolang_process_id() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int32_t>(::getpid());
#endif
}

extern "C" void wolang_sleep_micros(uint64_t micros) {
#if defined(_WIN32)
  ::Sleep(static_cast<DWORD>((micros + 999) / 1000));
#else
  timespec wanted;
  wanted.tv_sec = static_cast<time_t>(micros / 1000000);
  wanted.tv_nsec = static_cast<int64_t>(micros % 1000000) * 1000;
  timespec left;

  while (::nanosleep(&wanted, &left) != 0) {
    wanted = left;
  }
#endif
}

extern "C" int32_t wolang_clock(int32_t clock, int64_t* out) {
#if defined(_WIN32)
  if (clock == kRealtime) {
    FILETIME now;
    ::GetSystemTimePreciseAsFileTime(&now);
    const uint64_t ticks = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const uint64_t since = ticks - 116444736000000000ull;
    out[0] = static_cast<int64_t>(since / 10000000);
    out[1] = static_cast<int64_t>(since % 10000000) * 100;

    return 0;
  }

  constexpr int64_t kNanosPerSecond = 1000000000;
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  ::QueryPerformanceFrequency(&frequency);
  ::QueryPerformanceCounter(&counter);
  out[0] = counter.QuadPart / frequency.QuadPart;
  out[1] = (counter.QuadPart % frequency.QuadPart) * kNanosPerSecond / frequency.QuadPart;

  return 0;
#else
  timespec now;
  if (::clock_gettime(clock == kRealtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now) != 0) {
    return -1;
  }

  out[0] = static_cast<int64_t>(now.tv_sec);
  out[1] = static_cast<int64_t>(now.tv_nsec);

  return 0;
#endif
}

extern "C" int32_t wolang_system() {
#if defined(_WIN32)
  return 2;
#elif defined(__APPLE__)
  return 1;
#else
  return 0;
#endif
}

}  // namespace runtime
}  // namespace wolang
