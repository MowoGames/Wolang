// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/threads.h"

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>
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

extern "C" void wolang_parallel_for(uint64_t code,
                                    uint64_t environment,
                                    int64_t count,
                                    int32_t threads) {
  // What a data-parallel loop is: the range split into chunks, a thread
  // per chunk, and a join. The body is a function value -- code and the
  // environment it closed over -- so a chunk is `code(env, begin, end)`
  // when there is an environment and `code(begin, end)` when there is
  // not, which is the same rule a call through a value follows (D147).
  if (code == 0 || count <= 0) {
    return;
  }
  int32_t wanted = threads;
  if (wanted <= 0) {
    wanted = static_cast<int32_t>(wolang_thread_count());
  }
  if (wanted > count) {
    wanted = static_cast<int32_t>(count);
  }
  if (wanted < 1) {
    wanted = 1;
  }

  using WithEnvironment = void (*)(uint64_t, int64_t, int64_t);
  using Bare = void (*)(int64_t, int64_t);
  const auto with = reinterpret_cast<WithEnvironment>(code);
  const auto bare = reinterpret_cast<Bare>(code);

  const int64_t each = count / wanted;
  const int64_t extra = count % wanted;
  std::vector<std::thread> started;
  started.reserve(static_cast<size_t>(wanted) - 1);
  int64_t begin = 0;
  for (int32_t i = 0; i < wanted; ++i) {
    // The remainder goes to the first chunks, one each: a loop over a
    // hundred and one elements on eight threads should not leave one
    // thread with a hundred.
    const int64_t size = each + (i < extra ? 1 : 0);
    const int64_t end = begin + size;
    if (i + 1 == wanted) {
      // This thread takes the last chunk, so a one-thread loop spawns
      // nothing at all.
      if (environment != 0) {
        with(environment, begin, end);
      } else {
        bare(begin, end);
      }
      break;
    }
    started.emplace_back([=]() {
      if (environment != 0) {
        with(environment, begin, end);
      } else {
        bare(begin, end);
      }
    });
    begin = end;
  }
  for (std::thread& thread : started) {
    thread.join();
  }
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
