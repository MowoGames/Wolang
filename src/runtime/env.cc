// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/env.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wolang {
namespace runtime {
namespace {

std::vector<std::string>& Arguments() {
  static std::vector<std::string> arguments;
  return arguments;
}

// Values handed out stay alive for as long as the program does: a `text`
// in Wolang is a pointer and a length, and a pointer into a temporary
// would be a view of nothing.
std::vector<std::string>& Held() {
  static std::vector<std::string> held;
  return held;
}

}  // namespace

extern "C" void wolang_args_set(int32_t count,
                                const char* const* values) {
  Arguments().clear();
  for (int32_t i = 0; i < count && values != nullptr; ++i) {
    Arguments().emplace_back(values[i] != nullptr ? values[i] : "");
  }
}

extern "C" int32_t wolang_args_count(void) {
  return static_cast<int32_t>(Arguments().size());
}

extern "C" const uint8_t* wolang_args_at(int32_t index, uint64_t* length) {
  if (index < 0 || static_cast<size_t>(index) >= Arguments().size()) {
    if (length != nullptr) {
      *length = 0;
    }
    return nullptr;
  }
  const std::string& argument = Arguments()[static_cast<size_t>(index)];
  if (length != nullptr) {
    *length = argument.size();
  }
  return reinterpret_cast<const uint8_t*>(argument.data());
}

extern "C" const uint8_t* wolang_env_get(const uint8_t* name,
                                         uint64_t name_length,
                                         uint64_t* length) {
  if (length != nullptr) {
    *length = 0;
  }
  if (name == nullptr) {
    return nullptr;
  }
  // `getenv` wants a C string and a Wolang `text` is not one, so the name
  // is copied to ask with.
  const std::string wanted(reinterpret_cast<const char*>(name),
                           static_cast<size_t>(name_length));
  const char* found = std::getenv(wanted.c_str());
  if (found == nullptr) {
    return nullptr;
  }
  Held().emplace_back(found);
  const std::string& value = Held().back();
  if (length != nullptr) {
    *length = value.size();
  }
  return reinterpret_cast<const uint8_t*>(value.data());
}

extern "C" int32_t wolang_env_set(const uint8_t* name,
                                  uint64_t name_length,
                                  const uint8_t* value,
                                  uint64_t value_length) {
  if (name == nullptr || value == nullptr) {
    return -1;
  }
  const std::string wanted(reinterpret_cast<const char*>(name),
                           static_cast<size_t>(name_length));
  const std::string held(reinterpret_cast<const char*>(value),
                         static_cast<size_t>(value_length));
#if defined(_WIN32)
  // One call for setting and unsetting here, and the name is spelled
  // with an underscore (D184).
  return ::_putenv_s(wanted.c_str(), held.c_str()) == 0 ? 0 : -1;
#else
  return ::setenv(wanted.c_str(), held.c_str(), 1);
#endif
}

}  // namespace runtime
}  // namespace wolang
