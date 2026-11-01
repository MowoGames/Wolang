// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The embedding header, compiled as C11: if it stops being C, or a
// WolangValue changes size or layout, this file stops building (D241).

#include <stddef.h>

#include "wolang/embed/wolang.h"

_Static_assert(sizeof(WolangValue) == 32, "WolangValue is 32 bytes");
_Static_assert(offsetof(WolangValue, kind) == 0, "kind first");
_Static_assert(offsetof(WolangValue, reserved) == 4, "reserved second");
_Static_assert(offsetof(WolangValue, integer) == 8, "payload at 8");
_Static_assert(offsetof(WolangValue, number) == 8, "payload at 8");
_Static_assert(offsetof(WolangValue, boolean) == 8, "payload at 8");
_Static_assert(offsetof(WolangValue, handle) == 8, "payload at 8");
_Static_assert(offsetof(WolangValue, text) == 16, "text at 16");
_Static_assert(offsetof(WolangValue, length) == 24, "length at 24");
_Static_assert(sizeof(((WolangValue*)0)->kind) == 4, "kind is 32 bits");
_Static_assert(offsetof(WolangVmOptions, size) == 0, "size first");
_Static_assert(WOLANG_ABI_VERSION == ((1u << 16) | 1u), "ABI 1.1");

// Called from EmbedTest so the object is linked, and so the test sees the
// size the C compiler computed rather than the C++ one.
uint32_t wolang_abi_c_check_size(void) {
  return (uint32_t)sizeof(WolangValue);
}
