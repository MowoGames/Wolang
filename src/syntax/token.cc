// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/token.h"

namespace wolang {

std::string_view NumericSuffixName(NumericSuffix suffix) {
  switch (suffix) {
    case NumericSuffix::kNone:
      return "";
    case NumericSuffix::kI8:
      return "i8";
    case NumericSuffix::kI16:
      return "i16";
    case NumericSuffix::kI32:
      return "i32";
    case NumericSuffix::kI64:
      return "i64";
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
