// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/files.h"

#include <fcntl.h>

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
  return static_cast<int32_t>(::open(name.c_str(), O_RDONLY));
}

extern "C" int32_t wolang_open_write(const char* path, uint64_t length) {
  const std::string name = Terminated(path, length);
  return static_cast<int32_t>(
      ::open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
}

}  // namespace runtime
}  // namespace wolang
