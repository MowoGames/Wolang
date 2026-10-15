// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// A file a program keeps open, reads at an offset, appends to, and makes
// sure is really on the disk.
//
// `std.fs` reads and writes whole files, which is the right shape for a
// configuration file and the wrong one for a database: a store appends
// records and reads them back at the offset it remembers. So this is the
// handful of calls that needs -- open, read at, append, size, sync,
// truncate, close -- and nothing else (D180).

#include "wolang/runtime/handles.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstring>
#include <string>

namespace wolang {
namespace runtime {
namespace {

std::string Named(const uint8_t* path, uint64_t length) {
  if (path == nullptr || length == 0) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(path),
                     static_cast<size_t>(length));
}

}  // namespace

extern "C" {

int32_t wolang_file_open(const uint8_t* path, uint64_t length,
                         int32_t writable) {
  const std::string named = Named(path, length);
  if (named.empty()) {
    return -1;
  }
#if defined(_WIN32)
  const int flags = writable != 0
                        ? (_O_RDWR | _O_CREAT | _O_BINARY)
                        : (_O_RDONLY | _O_BINARY);
  return static_cast<int32_t>(
      ::_open(named.c_str(), flags, _S_IREAD | _S_IWRITE));
#else
  const int flags = writable != 0 ? (O_RDWR | O_CREAT) : O_RDONLY;
  return static_cast<int32_t>(::open(named.c_str(), flags, 0644));
#endif
}

int64_t wolang_file_read_at(int32_t handle, int64_t offset,
                            uint8_t* into, uint64_t count) {
  if (handle < 0 || into == nullptr || count == 0) {
    return -1;
  }
#if defined(_WIN32)
  if (::_lseeki64(handle, offset, SEEK_SET) < 0) {
    return -1;
  }
  return ::_read(handle, into, static_cast<unsigned>(count));
#else
  return ::pread(handle, into, static_cast<size_t>(count),
                 static_cast<off_t>(offset));
#endif
}

int64_t wolang_file_append(int32_t handle, const uint8_t* bytes,
                           uint64_t count) {
  if (handle < 0 || bytes == nullptr || count == 0) {
    return -1;
  }
#if defined(_WIN32)
  if (::_lseeki64(handle, 0, SEEK_END) < 0) {
    return -1;
  }
  return ::_write(handle, bytes, static_cast<unsigned>(count));
#else
  if (::lseek(handle, 0, SEEK_END) < 0) {
    return -1;
  }
  return ::write(handle, bytes, static_cast<size_t>(count));
#endif
}

int64_t wolang_file_size(int32_t handle) {
  if (handle < 0) {
    return -1;
  }
#if defined(_WIN32)
  const int64_t held = ::_lseeki64(handle, 0, SEEK_END);
#else
  const int64_t held = static_cast<int64_t>(::lseek(handle, 0, SEEK_END));
#endif
  return held;
}

int32_t wolang_file_sync(int32_t handle) {
  if (handle < 0) {
    return -1;
  }
#if defined(_WIN32)
  return ::_commit(handle) == 0 ? 0 : -1;
#else
  // `fsync` is what makes a write survive the machine losing power,
  // which is the whole difference between a store and a cache.
  return ::fsync(handle) == 0 ? 0 : -1;
#endif
}

int32_t wolang_file_truncate(int32_t handle, int64_t size) {
  if (handle < 0) {
    return -1;
  }
#if defined(_WIN32)
  return ::_chsize_s(handle, size) == 0 ? 0 : -1;
#else
  return ::ftruncate(handle, static_cast<off_t>(size)) == 0 ? 0 : -1;
#endif
}

int32_t wolang_file_close(int32_t handle) {
  if (handle < 0) {
    return 0;
  }
#if defined(_WIN32)
  return ::_close(handle) == 0 ? 0 : -1;
#else
  return ::close(handle) == 0 ? 0 : -1;
#endif
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
