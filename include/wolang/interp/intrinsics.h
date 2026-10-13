// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_INTERP_INTRINSICS_H_
#define WOLANG_INTERP_INTRINSICS_H_

#include <string>
#include <vector>

#include "wolang/interp/value.h"

namespace wolang {
namespace interp {

// The operations the language cannot express about itself: writing to the
// console, turning a number into text. A declaration with no body whose name
// matches one of these is implemented by the host rather than being an
// error.
//
// Declared in Wolang as a body-less `offer fun` in `std`, which is what
// `docs/roadmap.md` calls the platform floor. The list is deliberately tiny:
// everything that *can* be written in Wolang is.
enum class Intrinsic : uint8_t {
  kNone,
  kPrint,
  kConcatStr,
  kIntToStr,
  kFloatToStr,
  kBoolToStr,
};

Intrinsic LookupIntrinsic(std::string_view name);

// Where `Print` goes. Collected rather than written directly, so that a test
// can read what a program printed.
struct Output {
  std::string text;
};

// Runs |intrinsic|. Returns an undefined value for one it does not know,
// which the caller turns into a trap.
Value CallIntrinsic(Intrinsic intrinsic,
                    const std::vector<Value>& arguments,
                    Output* output);

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_INTRINSICS_H_
