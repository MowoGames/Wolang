// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/files.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <string>

namespace wolang {
namespace runtime {
namespace {

// A `text` is bytes and a length with no terminator, and `open` wants one
// -- so the path is copied. Paths are short and opening a file is a
// system call, so a copy here is free in the only sense that matters.
std::string Terminated(const char* path, uint64_t length) {
  return std::string(path, static_cast<size_t>(length));
}

}  // namespace

extern "C" int32_t wolang_open_read(const char* path, uint64_t length) {
  const std::string name = Terminated(path, length);
#if defined(_WIN32)
  // The same call under another name, and in binary: this system
  // otherwise turns a line break into two on the way out and back,
  // which is a file that does not round trip (D159).
  return static_cast<int32_t>(::_open(name.c_str(),
                                      _O_RDONLY | _O_BINARY));
#else
  return static_cast<int32_t>(::open(name.c_str(), O_RDONLY));
#endif
}

extern "C" int32_t wolang_open_write(const char* path, uint64_t length) {
  const std::string name = Terminated(path, length);
#if defined(_WIN32)
  return static_cast<int32_t>(
      ::_open(name.c_str(),
              _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
              _S_IREAD | _S_IWRITE));
#else
  return static_cast<int32_t>(
      ::open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
#endif
}

}  // namespace runtime
}  // namespace wolang
