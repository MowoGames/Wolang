// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The rest of a filesystem: what a path is, how big, when it changed,
// what is in a directory, and the verbs that change the tree (D149).

#include "wolang/runtime/files.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>

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

int32_t wolang_fs_kind(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return 0;
  }
  struct stat held = {};
  if (::stat(name, &held) != 0) {
    return 0;
  }
  if (S_ISREG(held.st_mode)) {
    return 1;
  }
  if (S_ISDIR(held.st_mode)) {
    return 2;
  }
  return 3;
}

int64_t wolang_fs_size(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  struct stat held = {};
  if (::stat(name, &held) != 0) {
    return -1;
  }
  return static_cast<int64_t>(held.st_size);
}

int64_t wolang_fs_changed(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  struct stat held = {};
  if (::stat(name, &held) != 0) {
    return -1;
  }
  return static_cast<int64_t>(held.st_mtime);
}

int32_t wolang_fs_remove(const uint8_t* path, uint64_t length) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  // A directory and a file are taken away by different calls, and which
  // one this is decides.
  struct stat held = {};
  if (::stat(name, &held) != 0) {
    return -1;
  }
  return S_ISDIR(held.st_mode) ? ::rmdir(name) : ::unlink(name);
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
  return ::rename(left, right);
}

int32_t wolang_fs_make_directory(const uint8_t* path,
                                 uint64_t length,
                                 int32_t parents) {
  char name[4096];
  if (!Terminated(path, length, name, sizeof(name))) {
    return -1;
  }
  if (parents == 0) {
    return ::mkdir(name, 0755) == 0 || errno == EEXIST ? 0 : -1;
  }
  // Every directory above it, in order: a path that exists already is
  // not a failure, which is what makes this safe to call twice.
  std::string held;
  for (size_t i = 0; name[i] != 0; ++i) {
    held.push_back(name[i]);
    if (name[i] != '/' || held.size() == 1) {
      continue;
    }
    if (::mkdir(held.c_str(), 0755) != 0 && errno != EEXIST) {
      return -1;
    }
  }
  if (::mkdir(name, 0755) != 0 && errno != EEXIST) {
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
  DIR* directory = ::opendir(name);
  if (directory == nullptr) {
    return -1;
  }
  uint64_t written = 0;
  // `.` and `..` are not entries anyone asked for.
  while (const dirent* entry = ::readdir(directory)) {
    const std::string_view held(entry->d_name);
    if (held == "." || held == "..") {
      continue;
    }
    if (written + held.size() + 1 > room) {
      ::closedir(directory);
      // Not an error the caller can act on except by asking for more
      // room, which is exactly what it does.
      return -1;
    }
    std::memcpy(into + written, held.data(), held.size());
    written += held.size();
    into[written++] = '\n';
  }
  ::closedir(directory);
  return static_cast<int64_t>(written);
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
