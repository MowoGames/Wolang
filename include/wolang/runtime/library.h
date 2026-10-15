// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Loading a library and finding a symbol in it.
//
// Three lines of `dlfcn.h` on every system that has one, and two Windows
// calls that mean the same thing. In one place, because the compiler,
// the interpreter and the driver all do it and none of them should have
// to know which system this is (D159).

#ifndef WOLANG_RUNTIME_LIBRARY_H_
#define WOLANG_RUNTIME_LIBRARY_H_

#include <string>

namespace wolang {
namespace runtime {

// Opens |path| and keeps it open for as long as the process runs.
// Answers whether it is there.
bool OpenLibrary(const std::string& path);

// The address of |name| anywhere in this process -- its own symbols and
// everything it has loaded. Null when there is none.
void* FindSymbol(const std::string& name);

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_LIBRARY_H_
