// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_CODEGEN_TARGET_H_
#define WOLANG_CODEGEN_TARGET_H_

#include <cstdint>
#include <string_view>

namespace wolang {
namespace codegen {

// Which machine the dev tier is emitting for.
//
// Everything above the emitter is target-independent on purpose: the IR, the
// passes, the layout, the trap table, and the transform that splits a `task`
// into a state machine. A new architecture is an assembler and an emitter,
// not a second compiler -- and the interpreter is always there as the
// reference to check it against.
enum class Target : uint8_t {
  kUnknown,
  kArm64,
  kX64,
};

// The machine this build is running on.
Target HostTarget();

// How a target is written in a message.
std::string_view TargetName(Target target);

// Whether this build can emit code for |target|.
bool HasBackend(Target target);

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_TARGET_H_
