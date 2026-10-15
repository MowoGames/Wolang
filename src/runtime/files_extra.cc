// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The rest of a filesystem: what a path is, how big, when it changed,
// what is in a directory, and the verbs that change the tree (D149).
//
// Through the standard library rather than through `dirent.h` and
// `stat`: those are the same everywhere except on the system that spells
// them differently, and one implementation that is right is better than
// two that have to agree (D159).

#include "wolang/runtime/files.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace wolang {
namespace runtime {
namespace {

// A view with a length into a C string, which is what every call here
// wants. Paths are short, so the copy is on the stack.
bool Terminated(const uint8_t* path, uint64_t length, char* into,
                size_t room) {
  if (path == nullptr || length == 0 || length >= room) {
    return false;
  }
  std::memcpy(into, path, static_cast<size_t>(length));
  into[length] = 0;
  return true;
}

}  // namespace

extern "C" {

namespace fs = std::filesystem;

int32_t wolang_fs_kind(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return 0;
  }
  std::error_code failed;
  const fs::file_status held = fs::status(fs::path(name), failed);
  if (failed) {
    return 0;
  }
  if (fs::is_regular_file(held)) {
    return 1;
  }
  if (fs::is_directory(held)) {
    return 2;
  }
  if (!fs::exists(held)) {
    return 0;
  }
  return 3;
}

int64_t wolang_fs_size(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  std::error_code failed;
  const uintmax_t size = fs::file_size(fs::path(name), failed);
  return failed ? -1 : static_cast<int64_t>(size);
}

int64_t wolang_fs_changed(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  std::error_code failed;
  const fs::file_time_type when =
      fs::last_write_time(fs::path(name), failed);
  if (failed) {
    return -1;
  }
  // A file's clock is not the wall clock on every system, which is what
  // `to_sys` is for: the answer is seconds since the epoch everywhere.
  const auto wall = std::chrono::file_clock::to_sys(when);
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          wall.time_since_epoch())
          .count());
}

int32_t wolang_fs_remove(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  std::error_code failed;
  // One file or one empty directory: taking a tree away is something a
  // program should have to say it means, and `Remove` does not say it.
  return fs::remove(fs::path(name), failed) && !failed ? 0 : -1;
}

int32_t wolang_fs_rename(const uint8_t* from,
                         uint64_t from_length,
                         const uint8_t* to,
                         uint64_t to_length) {
  char left[4096];
  char right[4096];
  if (!Terminated(from, from_length, left, sizeof(left)) ||
      !Terminated(to, to_length, right, sizeof(right))) {
    return -1;
  }
  std::error_code failed;
  fs::rename(fs::path(left), fs::path(right), failed);
  return failed ? -1 : 0;
}

int32_t wolang_fs_make_directory(const uint8_t* path,
                                 uint64_t length,
                                 int32_t parents) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  std::error_code failed;
  const fs::path where(name);
  if (parents != 0) {
    fs::create_directories(where, failed);
  } else {
    fs::create_directory(where, failed);
  }
  // A directory that is already there is not a failure, which is what
  // makes this safe to call twice.
  if (failed && !fs::is_directory(where)) {
    return -1;
  }
  return 0;
}

int64_t wolang_fs_list(const uint8_t* path,
                       uint64_t length,
                       uint8_t* into,
                       uint64_t room) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name)) || into == nullptr) {
    return -1;
  }
  std::error_code failed;
  fs::directory_iterator walk(fs::path(name), failed);
  if (failed) {
    return -1;
  }
  uint64_t written = 0;
  for (const fs::directory_entry& entry : walk) {
    const std::string held = entry.path().filename().string();
    if (held.empty() || held == "." || held == "..") {
      continue;
    }
    if (written + held.size() + 1 > room) {
      // Not an error the caller can act on except by asking for more
      // room, which is exactly what it does.
      return -1;
    }
    std::memcpy(into + written, held.data(), held.size());
    written += held.size();
    into[written++] = '\n';
  }
  return static_cast<int64_t>(written);
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
