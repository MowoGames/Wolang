// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// What every backend needs and none of them should write twice: a queue
// of events, a ring of audio frames, and a clock.

#ifndef WOLANG_PLATFORM_COMMON_H_
#define WOLANG_PLATFORM_COMMON_H_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "wolang/platform/platform.h"

namespace wolang {
namespace platform {

// A queue of events, written by whatever the system calls and read by
// `wolang_window_poll`.
//
// Bounded and oldest-dropped: a program that stops polling has a bug,
// and growing without limit turns that bug into a dead machine.
class Events {
 public:
  static constexpr size_t kRoom = 256;

  void Push(const WolangEvent& event) {
    if (held_.size() >= kRoom) {
      held_.erase(held_.begin());
    }
    held_.push_back(event);
  }

  bool Take(WolangEvent* out) {
    if (held_.empty()) {
      return false;
    }
    *out = held_.front();
    held_.erase(held_.begin());
    return true;
  }

  bool empty() const { return held_.empty(); }

 private:
  std::vector<WolangEvent> held_;
};

// Frames waiting to be played.
//
// One writer -- the program's frame loop -- and one reader, which is the
// system's audio thread: a lock there is a glitch waiting to happen, so
// this is two indices and nothing else.
class Ring {
 public:
  void Open(int32_t frames, int32_t channels) {
    channels_ = channels < 1 ? 1 : channels;
    samples_.assign(static_cast<size_t>(frames) * channels_, 0.0f);
    read_.store(0);
    write_.store(0);
  }

  int32_t Frames() const {
    return static_cast<int32_t>(samples_.size() / channels_);
  }

  int32_t Queued() const {
    const size_t write = write_.load(std::memory_order_acquire);
    const size_t read = read_.load(std::memory_order_acquire);
    return static_cast<int32_t>((write - read) / channels_);
  }

  int32_t Room() const { return Frames() - 1 - Queued(); }

  // Adds up to |frames| and answers how many it took.
  int32_t Write(const float* samples, int32_t frames) {
    const int32_t room = Room();
    const int32_t taking = frames < room ? frames : room;
    if (taking <= 0) {
      return 0;
    }
    size_t write = write_.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < taking * channels_; ++i) {
      samples_[write % samples_.size()] = samples[i];
      ++write;
    }
    write_.store(write, std::memory_order_release);
    return taking;
  }

  // Fills |into| with what is queued, and silence past it: an output
  // that is short of frames has to play something, and silence is the
  // only honest something.
  void Read(float* into, int32_t frames) {
    size_t read = read_.load(std::memory_order_relaxed);
    const size_t write = write_.load(std::memory_order_acquire);
    for (int32_t i = 0; i < frames * channels_; ++i) {
      if (read < write) {
        into[i] = samples_[read % samples_.size()];
        ++read;
      } else {
        into[i] = 0.0f;
      }
    }
    read_.store(read, std::memory_order_release);
  }

  int32_t channels() const { return channels_; }

 private:
  std::vector<float> samples_;
  // Counted in samples rather than frames, and never wrapped: the
  // difference is what is queued, and a 64-bit count does not come back
  // around in any run that will ever happen.
  std::atomic<size_t> read_{0};
  std::atomic<size_t> write_{0};
  int32_t channels_ = 2;
};

// An event with everything zeroed, so that a backend fills what it means
// and nothing else.
inline WolangEvent Blank(int32_t kind) {
  WolangEvent event;
  std::memset(&event, 0, sizeof(event));
  event.kind = kind;
  return event;
}

// Seconds from a clock that does not jump.
double Monotonic();

}  // namespace platform
}  // namespace wolang

#endif  // WOLANG_PLATFORM_COMMON_H_
