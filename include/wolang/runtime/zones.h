// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_ZONES_H_
#define WOLANG_RUNTIME_ZONES_H_

#include <cstdint>

namespace wolang {
namespace runtime {

extern "C" {

// How many seconds |zone| is ahead of UTC at |seconds|, which depends on
// the moment: summer time is a thing governments do. An empty zone means
// the machine's own (D176).
int32_t wolang_zone_offset(int64_t seconds, const char* zone,
                           uint64_t length);

// What the zone is called at that moment -- `CET`, `PDT` -- and how many
// bytes that is.
int32_t wolang_zone_name(int64_t seconds, const char* zone,
                         uint64_t length, uint8_t* into, uint64_t room);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_ZONES_H_
