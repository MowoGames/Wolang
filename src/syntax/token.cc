// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/syntax/token.h"

namespace wolang {

std::string_view NumericSuffixName(NumericSuffix suffix) {
  switch (suffix) {
    case NumericSuffix::kNone:
      return "";
    case NumericSuffix::kI8:
      return "s8";
    case NumericSuffix::kI16:
      return "s16";
    case NumericSuffix::kI32:
      return "s32";
    case NumericSuffix::kI64:
      return "s64";
    case NumericSuffix::kU8:
      return "u8";
    case NumericSuffix::kU16:
      return "u16";
    case NumericSuffix::kU32:
      return "u32";
    case NumericSuffix::kU64:
      return "u64";
    case NumericSuffix::kF32:
      return "f32";
    case NumericSuffix::kF64:
      return "f64";
  }
  return "";
}

}  // namespace wolang
