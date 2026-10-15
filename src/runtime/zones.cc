// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// What the system thinks a time zone is.
//
// A zone is not arithmetic: it is a database of what each government
// decided, revised several times a year, and every system already has a
// copy that everything else on the machine agrees with. So this asks it
// -- `localtime_r` and `timegm` -- rather than shipping a table that
// would be wrong by the time anyone ran it (D176).

#include "wolang/runtime/zones.h"

#include <ctime>
#include <cstdlib>
#include <cstring>
#include <string>

namespace wolang {
namespace runtime {
namespace {

// The zone a name says, set for this thread's next conversion.
//
// `TZ` and `tzset` is the one interface every system has for "what would
// this moment be called over there". Not thread-safe by nature, so the
// whole conversion happens under one lock.
struct HeldZone {
  std::string previous;
  bool had_previous = false;
};

HeldZone Enter(const char* zone) {
  HeldZone held;
  if (zone == nullptr || zone[0] == 0) {
    return held;
  }
  if (const char* existing = std::getenv("TZ"); existing != nullptr) {
    held.previous = existing;
    held.had_previous = true;
  }
#if defined(_WIN32)
  ::_putenv_s("TZ", zone);
  ::_tzset();
#else
  ::setenv("TZ", zone, 1);
  ::tzset();
#endif
  return held;
}

void Leave(const HeldZone& held, const char* zone) {
  if (zone == nullptr || zone[0] == 0) {
    return;
  }
#if defined(_WIN32)
  ::_putenv_s("TZ", held.had_previous ? held.previous.c_str() : "");
  ::_tzset();
#else
  if (held.had_previous) {
    ::setenv("TZ", held.previous.c_str(), 1);
  } else {
    ::unsetenv("TZ");
  }
  ::tzset();
#endif
}

}  // namespace

extern "C" int32_t wolang_zone_offset(int64_t seconds, const char* zone,
                                      uint64_t length) {
  std::string named(zone != nullptr ? zone : "",
                    static_cast<size_t>(length));
  const HeldZone held = Enter(named.c_str());
  const auto moment = static_cast<std::time_t>(seconds);
  std::tm local = {};
#if defined(_WIN32)
  ::localtime_s(&local, &moment);
#else
  ::localtime_r(&moment, &local);
#endif
  // What the same wall clock would be as a moment if it were in UTC:
  // the difference is the offset, which is what a caller wants and what
  // no portable function answers directly.
  std::tm copy = local;
  copy.tm_isdst = 0;
#if defined(_WIN32)
  const std::time_t as_utc = ::_mkgmtime(&copy);
#else
  const std::time_t as_utc = ::timegm(&copy);
#endif
  Leave(held, named.c_str());
  return static_cast<int32_t>(as_utc - moment);
}

extern "C" int32_t wolang_zone_name(int64_t seconds, const char* zone,
                                    uint64_t length, uint8_t* into,
                                    uint64_t room) {
  if (into == nullptr || room == 0) {
    return -1;
  }
  std::string named(zone != nullptr ? zone : "",
                    static_cast<size_t>(length));
  const HeldZone held = Enter(named.c_str());
  const auto moment = static_cast<std::time_t>(seconds);
  std::tm local = {};
#if defined(_WIN32)
  ::localtime_s(&local, &moment);
#else
  ::localtime_r(&moment, &local);
#endif
  char buffer[64] = {};
  const size_t written = std::strftime(buffer, sizeof(buffer), "%Z",
                                       &local);
  Leave(held, named.c_str());
  const uint64_t size = written < room ? written : room;
  std::memcpy(into, buffer, static_cast<size_t>(size));
  return static_cast<int32_t>(size);
}

}  // namespace runtime
}  // namespace wolang
