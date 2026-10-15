// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Running a program and reading what it said, under the name each
// system gives it.
//
// Three places in the compiler do this -- `wolang bind` calling the C
// preprocessor, the workspace calling `git`, and the driver running a
// test binary -- and Windows spells both calls with an underscore
// (D184).

#ifndef WOLANG_BASE_SHELL_H_
#define WOLANG_BASE_SHELL_H_

#include <cstdio>

#if defined(_WIN32)
#define WOLANG_POPEN _popen
#define WOLANG_PCLOSE _pclose
#else
#define WOLANG_POPEN popen
#define WOLANG_PCLOSE pclose
#endif

#endif  // WOLANG_BASE_SHELL_H_
