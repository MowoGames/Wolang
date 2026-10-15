// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/journal.h"

#include "wolang/runtime/builtins.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace wolang {
namespace runtime {
namespace {

// One entry: which call, and what it answered.
//
// The payload is the bytes the call handed back -- a `timespec`, the
// buffer a `read` filled, a process id -- and the length says how many.
// A text format, because a journal that cannot be read in an editor is a
// journal nobody looks at when the replay disagrees (D142).
struct State {
  std::FILE* file = nullptr;
  bool recording = false;
  bool replaying = false;
  // What a replay has left to hand out, oldest first.
  std::vector<std::pair<std::string, std::string>> entries;
  size_t at = 0;
  std::mutex lock;
};

State& Journal() {
  static State state;
  return state;
}

// Bytes as hexadecimal, because a journal holds whatever a buffer held.
std::string ToHex(const void* bytes, size_t length) {
  static const char kDigits[] = "0123456789abcdef";
  const auto* at = static_cast<const uint8_t*>(bytes);
  std::string out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[at[i] >> 4]);
    out.push_back(kDigits[at[i] & 0xF]);
  }
  return out;
}

size_t FromHex(const std::string& text, void* into, size_t room) {
  size_t written = 0;
  auto* out = static_cast<uint8_t*>(into);
  for (size_t i = 0; i + 1 < text.size() && written < room; i += 2) {
    const auto high = static_cast<uint8_t>(
        text[i] <= '9' ? text[i] - '0' : (text[i] | 32) - 'a' + 10);
    const auto low = static_cast<uint8_t>(
        text[i + 1] <= '9' ? text[i + 1] - '0'
                           : (text[i + 1] | 32) - 'a' + 10);
    out[written++] = static_cast<uint8_t>((high << 4) | low);
  }
  return written;
}

void Write(const char* what, const void* bytes, size_t length) {
  State& state = Journal();
  if (!state.recording || state.file == nullptr) {
    return;
  }
  const std::string hex = ToHex(bytes, length);
  std::fprintf(state.file, "%s %s\n", what, hex.c_str());
}

// The next entry for |what|, or nothing.
//
// In order, and the name has to match: a replay whose calls happen in a
// different order is a replay of a different program, and saying so
// beats handing back a timestamp where a buffer was wanted.
bool Next(const char* what, std::string* payload) {
  State& state = Journal();
  if (!state.replaying || state.at >= state.entries.size()) {
    return false;
  }
  const auto& [name, held] = state.entries[state.at];
  if (name != what) {
    return false;
  }
  *payload = held;
  ++state.at;
  return true;
}

}  // namespace

int32_t JournalRecord(const char* path) {
  State& state = Journal();
  std::lock_guard<std::mutex> held(state.lock);
  JournalClose();
  state.file = std::fopen(path, "w");
  if (state.file == nullptr) {
    return 1;
  }
  state.recording = true;
  state.replaying = false;
  // The shims take over the calls the standard library declares: the
  // registry is consulted before the loader, so a program reaches these
  // without knowing (D137).
  RegisterSymbol("clock_gettime",
                 reinterpret_cast<void*>(&wolang_journal_clock));
  RegisterSymbol("read", reinterpret_cast<void*>(&wolang_journal_read));
  RegisterSymbol("getpid",
                 reinterpret_cast<void*>(&wolang_journal_getpid));
  return 0;
}

int32_t JournalReplay(const char* path) {
  State& state = Journal();
  std::lock_guard<std::mutex> held(state.lock);
  JournalClose();
  std::FILE* file = std::fopen(path, "r");
  if (file == nullptr) {
    return 1;
  }
  char line[1 << 16];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }
    const size_t space = text.find(' ');
    if (space == std::string::npos) {
      continue;
    }
    state.entries.emplace_back(text.substr(0, space),
                               text.substr(space + 1));
  }
  std::fclose(file);
  state.at = 0;
  state.replaying = true;
  state.recording = false;
  RegisterSymbol("clock_gettime",
                 reinterpret_cast<void*>(&wolang_journal_clock));
  RegisterSymbol("read", reinterpret_cast<void*>(&wolang_journal_read));
  RegisterSymbol("getpid",
                 reinterpret_cast<void*>(&wolang_journal_getpid));
  return 0;
}

void JournalClose() {
  State& state = Journal();
  if (state.file != nullptr) {
    std::fclose(state.file);
    state.file = nullptr;
  }
  state.recording = false;
  state.replaying = false;
  state.entries.clear();
  state.at = 0;
}

bool JournalRecording() {
  return Journal().recording;
}

bool JournalReplaying() {
  return Journal().replaying;
}

extern "C" {

int32_t wolang_journal_clock(int32_t clock, void* out) {
  // Two 64-bit numbers, which is what a `timespec` is on every system
  // this targets.
  State& state = Journal();
  std::lock_guard<std::mutex> held(state.lock);
  if (state.replaying) {
    std::string payload;
    if (Next("clock", &payload)) {
      FromHex(payload, out, 16);
      return 0;
    }
    // Nothing left: the program asked more than the recording holds, so
    // the clock stops rather than drifting.
    std::memset(out, 0, 16);
    return 0;
  }
  const int32_t status =
      ::clock_gettime(static_cast<clockid_t>(clock),
                      static_cast<struct timespec*>(out));
  Write("clock", out, 16);
  return status;
}

int64_t wolang_journal_read(int32_t fd, void* buffer, uint64_t count) {
  State& state = Journal();
  std::lock_guard<std::mutex> held(state.lock);
  if (state.replaying) {
    std::string payload;
    if (!Next("read", &payload)) {
      return 0;
    }
    const size_t written = FromHex(payload, buffer, count);
    return static_cast<int64_t>(written);
  }
  const int64_t received = ::read(fd, buffer, count);
  Write("read", buffer, received > 0 ? static_cast<size_t>(received) : 0);
  return received;
}

int32_t wolang_journal_getpid(void) {
  State& state = Journal();
  std::lock_guard<std::mutex> held(state.lock);
  if (state.replaying) {
    std::string payload;
    int32_t held_id = 0;
    if (Next("pid", &payload)) {
      FromHex(payload, &held_id, sizeof(held_id));
    }
    return held_id;
  }
  const int32_t id = ::getpid();
  Write("pid", &id, sizeof(id));
  return id;
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
