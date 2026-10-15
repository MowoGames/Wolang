// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/library.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>
#else
#include <dlfcn.h>
#endif

namespace wolang {
namespace runtime {

#if defined(_WIN32)
namespace {

// What has been loaded, so that a symbol can be looked for in all of it:
// this system has no `RTLD_DEFAULT`, and asking each module in turn is
// what that flag does elsewhere.
std::vector<HMODULE>& Loaded() {
  static std::vector<HMODULE> loaded;
  return loaded;
}

}  // namespace

bool OpenLibrary(const std::string& path) {
  const HMODULE module = ::LoadLibraryA(path.c_str());
  if (module == nullptr) {
    return false;
  }
  Loaded().push_back(module);
  return true;
}

void* FindSymbol(const std::string& name) {
  // This process first, then everything it has loaded, then the system's
  // own libraries -- which is the order the loader would have used.
  if (const HMODULE self = ::GetModuleHandleA(nullptr); self != nullptr) {
    if (FARPROC found = ::GetProcAddress(self, name.c_str());
        found != nullptr) {
      return reinterpret_cast<void*>(found);
    }
  }
  for (HMODULE module : Loaded()) {
    if (FARPROC found = ::GetProcAddress(module, name.c_str());
        found != nullptr) {
      return reinterpret_cast<void*>(found);
    }
  }
  static const char* kAlways[] = {"msvcrt.dll", "kernel32.dll",
                                  "ws2_32.dll", "user32.dll"};
  for (const char* which : kAlways) {
    const HMODULE module = ::GetModuleHandleA(which);
    if (module == nullptr) {
      continue;
    }
    if (FARPROC found = ::GetProcAddress(module, name.c_str());
        found != nullptr) {
      return reinterpret_cast<void*>(found);
    }
  }
  return nullptr;
}

#else

bool OpenLibrary(const std::string& path) {
  return ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL) != nullptr;
}

void* FindSymbol(const std::string& name) {
  return ::dlsym(RTLD_DEFAULT, name.c_str());
}

#endif

}  // namespace runtime
}  // namespace wolang
