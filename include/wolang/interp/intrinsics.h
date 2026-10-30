// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_INTERP_INTRINSICS_H_
#define WOLANG_INTERP_INTRINSICS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wolang/interp/value.h"

namespace wolang {
namespace interp {

// The operations the language cannot express about itself.
//
// Two kinds live here, and they are here for different reasons. `Print` is
// a fallback for a file compiled on its own: a single file links no
// standard library, and a language whose `Print` stops working outside a
// package is a worse language to try things in.
//
// The rest are the string floor the runtime provides. The compiled tier
// reaches them through the C boundary; the interpreter calls the same
// functions directly, so that building a string needs no thunk and no
// backend -- the reference tier has to work on a machine that has neither.
//
// The list is deliberately tiny: everything that *can* be written in
// Wolang is.
enum class Intrinsic : uint8_t {
  kNone,
  kPrint,
  kTextFromInt,
  kTextFromUint,
  kFmod,
  kTextFromFloat,
  kTextFromBool,
  kTextConcat,
  kTextEqual,
  kTextFree,
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
