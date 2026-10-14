// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_RUNTIME_H_
#define WOLANG_CODEGEN_RUNTIME_H_

#include <cstddef>
#include <cstdint>

namespace wolang {
namespace codegen {

// The one thing generated code knows about the host.
//
// A compiled function keeps this address in x28 for its whole life. x28 is
// callee saved, so a call preserves it with nobody saving anything -- which
// is what makes a trap reportable from arbitrarily deep in compiled code
// without a check after every call.
struct Context {
  uint32_t trap_kind = 0;
  uint32_t trap_site = 0;
  // The `jmp_buf` of the call that entered compiled code. A trap is a
  // non-local exit: generated frames hold no objects that need unwinding,
  // so leaving through them costs nothing and the hot path pays nothing for
  // the possibility.
  void* landing = nullptr;
  // What generated code calls to report a trap. Reached through the context
  // rather than by address, because a `bl` reaches 128 megabytes and
  // nothing promises the host is that close.
  void (*report)(Context*, uint32_t, uint32_t) = nullptr;
};

constexpr uint32_t kContextTrapKindOffset = 0;
constexpr uint32_t kContextTrapSiteOffset = 4;
constexpr uint32_t kContextLandingOffset = 8;
constexpr uint32_t kContextReportOffset = 16;

static_assert(offsetof(Context, trap_kind) == kContextTrapKindOffset);
static_assert(offsetof(Context, trap_site) == kContextTrapSiteOffset);
static_assert(offsetof(Context, landing) == kContextLandingOffset);
static_assert(offsetof(Context, report) == kContextReportOffset);

// Records the trap and leaves through |context->landing|. Never returns.
[[noreturn]] void ReportTrap(Context* context, uint32_t kind, uint32_t site);

// How a compiled function is entered: the trampoline the emitter generates
// in front of every one of them. |arguments| holds one 64-bit slot per
// parameter -- the bits of a scalar, or the address of an aggregate -- and
// |result| is where the return value is written.
using EntryPoint = void (*)(Context* context,
                            const uint64_t* arguments,
                            void* result);

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_RUNTIME_H_
