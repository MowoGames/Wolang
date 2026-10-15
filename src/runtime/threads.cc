// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/threads.h"

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace wolang {
namespace runtime {
namespace {

struct Started {
  void (*body)(void*) = nullptr;
  void* argument = nullptr;
};

void* Entry(void* raw) {
  Started* started = static_cast<Started*>(raw);
  if (started->body != nullptr) {
    started->body(started->argument);
  }
  delete started;
  return nullptr;
}

// Threads and locks are handed out by handle so that a Wolang value is a
// number in every tier. The tables only grow, which is what makes a
// handle valid for as long as the program runs.
std::mutex& TableLock() {
  static std::mutex lock;
  return lock;
}

std::vector<pthread_t>& Threads() {
  static std::vector<pthread_t> threads;
  return threads;
}

std::vector<pthread_mutex_t*>& Mutexes() {
  static std::vector<pthread_mutex_t*> mutexes;
  return mutexes;
}

}  // namespace

extern "C" uint64_t wolang_thread_spawn(void (*body)(void*),
                                        void* argument) {
  auto* started = new Started{body, argument};
  pthread_t thread;
  if (::pthread_create(&thread, nullptr, &Entry, started) != 0) {
    delete started;
    return 0;
  }
  const std::lock_guard<std::mutex> held(TableLock());
  Threads().push_back(thread);
  return Threads().size();
}

extern "C" int32_t wolang_thread_join(uint64_t handle) {
  pthread_t thread;
  {
    const std::lock_guard<std::mutex> held(TableLock());
    if (handle == 0 || handle > Threads().size()) {
      return -1;
    }
    thread = Threads()[handle - 1];
  }
  return ::pthread_join(thread, nullptr) == 0 ? 0 : -1;
}

extern "C" int32_t wolang_thread_count(void) {
  const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? static_cast<int32_t>(count) : 1;
}

extern "C" uint64_t wolang_mutex_create(void) {
  auto* mutex = new pthread_mutex_t;
  ::pthread_mutex_init(mutex, nullptr);
  const std::lock_guard<std::mutex> held(TableLock());
  Mutexes().push_back(mutex);
  return Mutexes().size();
}

extern "C" void wolang_mutex_destroy(uint64_t handle) {
  const std::lock_guard<std::mutex> held(TableLock());
  if (handle == 0 || handle > Mutexes().size()) {
    return;
  }
  pthread_mutex_t* mutex = Mutexes()[handle - 1];
  if (mutex == nullptr) {
    return;
  }
  ::pthread_mutex_destroy(mutex);
  delete mutex;
  Mutexes()[handle - 1] = nullptr;
}

extern "C" void wolang_mutex_lock(uint64_t handle) {
  pthread_mutex_t* mutex = nullptr;
  {
    const std::lock_guard<std::mutex> held(TableLock());
    if (handle == 0 || handle > Mutexes().size()) {
      return;
    }
    mutex = Mutexes()[handle - 1];
  }
  if (mutex != nullptr) {
    ::pthread_mutex_lock(mutex);
  }
}

extern "C" void wolang_mutex_unlock(uint64_t handle) {
  pthread_mutex_t* mutex = nullptr;
  {
    const std::lock_guard<std::mutex> held(TableLock());
    if (handle == 0 || handle > Mutexes().size()) {
      return;
    }
    mutex = Mutexes()[handle - 1];
  }
  if (mutex != nullptr) {
    ::pthread_mutex_unlock(mutex);
  }
}

extern "C" int64_t wolang_atomic_add(int64_t* address, int64_t delta) {
  if (address == nullptr) {
    return 0;
  }
  auto* counter = reinterpret_cast<std::atomic<int64_t>*>(address);
  return counter->fetch_add(delta, std::memory_order_relaxed);
}

}  // namespace runtime
}  // namespace wolang
