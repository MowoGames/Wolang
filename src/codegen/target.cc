// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/target.h"

namespace wolang {
namespace codegen {

Target HostTarget() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return Target::kArm64;
#elif defined(__x86_64__) || defined(_M_X64)
  return Target::kX64;
#else
  return Target::kUnknown;
#endif
}

std::string_view TargetName(Target target) {
  switch (target) {
    case Target::kArm64:
      return "arm64";
    case Target::kX64:
      return "x64";
    case Target::kUnknown:
      break;
  }
  return "an unknown machine";
}

bool HasBackend(Target target) {
  switch (target) {
    case Target::kArm64:
    case Target::kX64:
      return true;
    case Target::kUnknown:
      return false;
  }
  return false;
}

}  // namespace codegen
}  // namespace wolang
