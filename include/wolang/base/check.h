// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_BASE_CHECK_H_
#define WOLANG_BASE_CHECK_H_

#include <cstdio>
#include <cstdlib>

// Assertion macros. The project builds without exceptions, so a failed
// invariant aborts the process after printing where it happened.
//
//   WOLANG_CHECK(cond)      always compiled in
//   WOLANG_DCHECK(cond)     compiled out in release builds
//   WOLANG_NOTREACHED()     marks an unreachable branch

namespace wolang::internal {

[[noreturn]] inline void CheckFailed(const char* file,
                                     int line,
                                     const char* expression) {
  std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
  std::abort();
}

}  // namespace wolang::internal

#define WOLANG_CHECK(cond)                                                \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ::wolang::internal::CheckFailed(__FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (false)

#define WOLANG_NOTREACHED()                                               \
  ::wolang::internal::CheckFailed(__FILE__, __LINE__, "NOTREACHED")

#if defined(NDEBUG)
// The condition is compiled but never evaluated, so helpers used only by
// debug assertions do not become unused in release builds.
#define WOLANG_DCHECK(cond)  \
  do {                       \
    (void)sizeof((cond));    \
  } while (false)
#else
#define WOLANG_DCHECK(cond) WOLANG_CHECK(cond)
#endif

#endif  // WOLANG_BASE_CHECK_H_
