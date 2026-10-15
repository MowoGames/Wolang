// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The rest of what a program does to a filesystem: where it is, where
// the person's things are, what a path really is, copying, appending,
// and a temporary file that is actually unique.
//
// Through the standard library's filesystem where it has one, and
// through the system only for the three questions it does not answer:
// the working directory, the home directory and a temporary one (D179).

#include "wolang/runtime/files.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace wolang {
namespace runtime {
namespace {

namespace fs = std::filesystem;

std::string NamedPath(const uint8_t* path, uint64_t length) {
  if (path == nullptr || length == 0) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(path),
                     static_cast<size_t>(length));
}

// Copies |from| into |into|, and answers how many bytes that was, or
// -1. A caller that asked for more room than it had gets -1 and asks
// again, which is the same bargain `wolang_fs_list` makes.
int64_t Give(const std::string& from, uint8_t* into, uint64_t room) {
  if (into == nullptr || from.size() > room) {
    return -1;
  }
  std::memcpy(into, from.data(), from.size());
  return static_cast<int64_t>(from.size());
}

}  // namespace

extern "C" {

int64_t wolang_fs_current(uint8_t* into, uint64_t room) {
  std::error_code failed;
  const fs::path here = fs::current_path(failed);
  if (failed) {
    return -1;
  }
  return Give(here.string(), into, room);
}

int32_t wolang_fs_set_current(const uint8_t* path, uint64_t length) {
  std::error_code failed;
  fs::current_path(fs::path(NamedPath(path, length)), failed);
  return failed ? -1 : 0;
}

int64_t wolang_fs_home(uint8_t* into, uint64_t room) {
#if defined(_WIN32)
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (home == nullptr) {
    return -1;
  }
  return Give(std::string(home), into, room);
}

int64_t wolang_fs_temp_directory(uint8_t* into, uint64_t room) {
  std::error_code failed;
  const fs::path held = fs::temp_directory_path(failed);
  if (failed) {
    return -1;
  }
  return Give(held.string(), into, room);
}

int64_t wolang_fs_absolute(const uint8_t* path, uint64_t length,
                           uint8_t* into, uint64_t room) {
  std::error_code failed;
  // Weakly: a path that does not exist yet still has an absolute form,
  // which is what a program building an output path wants.
  const fs::path held =
      fs::weakly_canonical(fs::path(NamedPath(path, length)), failed);
  if (failed) {
    return -1;
  }
  return Give(held.string(), into, room);
}

int32_t wolang_fs_copy(const uint8_t* from, uint64_t from_length,
                       const uint8_t* to, uint64_t to_length,
                       int32_t over) {
  std::error_code failed;
  const fs::copy_options options =
      over != 0 ? fs::copy_options::overwrite_existing
                : fs::copy_options::none;
  fs::copy_file(fs::path(NamedPath(from, from_length)),
                fs::path(NamedPath(to, to_length)), options, failed);
  return failed ? -1 : 0;
}

int32_t wolang_fs_remove_tree(const uint8_t* path, uint64_t length) {
  std::error_code failed;
  const uintmax_t taken =
      fs::remove_all(fs::path(NamedPath(path, length)), failed);
  if (failed) {
    return -1;
  }
  return static_cast<int32_t>(taken > 0 ? 0 : -1);
}

// Every path under |path|, one per line, deepest last. The same bargain
// as `wolang_fs_list`: too little room is -1 (D149).
int64_t wolang_fs_walk(const uint8_t* path, uint64_t length,
                       uint8_t* into, uint64_t room) {
  std::error_code failed;
  fs::recursive_directory_iterator walk(fs::path(NamedPath(path, length)),
                                        failed);
  if (failed) {
    return -1;
  }
  std::string held;
  for (const fs::directory_entry& entry : walk) {
    held.append(entry.path().string());
    held.push_back('\n');
    if (held.size() > room) {
      return -1;
    }
  }
  return Give(held, into, room);
}

int32_t wolang_fs_append(const uint8_t* path, uint64_t length,
                         const uint8_t* bytes, uint64_t count) {
  const std::string named = NamedPath(path, length);
  std::FILE* file = std::fopen(named.c_str(), "ab");
  if (file == nullptr) {
    return -1;
  }
  const size_t written =
      bytes == nullptr || count == 0
          ? 0
          : std::fwrite(bytes, 1, static_cast<size_t>(count), file);
  std::fclose(file);
  return written == static_cast<size_t>(count) ? 0 : -1;
}

// A name nothing else has, inside the system's temporary directory.
//
// The process id and a counter: two programs started at the same moment
// have different ids, and one program asking twice gets two names.
int64_t wolang_fs_temp_name(const uint8_t* prefix, uint64_t length,
                            uint8_t* into, uint64_t room) {
  static int counter = 0;
  std::error_code failed;
  fs::path held = fs::temp_directory_path(failed);
  if (failed) {
    return -1;
  }
#if defined(_WIN32)
  const unsigned long id = ::GetCurrentProcessId();
#else
  const unsigned long id = static_cast<unsigned long>(::getpid());
#endif
  std::string name = NamedPath(prefix, length);
  if (name.empty()) {
    name = "wolang";
  }
  name.append("-");
  name.append(std::to_string(id));
  name.append("-");
  name.append(std::to_string(++counter));
  held /= name;
  return Give(held.string(), into, room);
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
