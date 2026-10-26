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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

#if defined(_WIN32)
struct Icu {
  void* (*Open)(const char16_t*, int32_t, const char*, int32_t, int32_t*) = nullptr;
  void (*SetMillis)(void*, double, int32_t*) = nullptr;
  int32_t (*Get)(const void*, int32_t, int32_t*) = nullptr;
  int8_t (*InDaylight)(const void*, int32_t*) = nullptr;
  int32_t (*DisplayName)(const void*, int32_t, const char*, char16_t*, int32_t, int32_t*) = nullptr;
  void (*Close)(void*) = nullptr;
};

static constexpr int32_t kGregorian = 1;
static constexpr int32_t kZoneOffsetField = 15;
static constexpr int32_t kDstOffsetField = 16;
static constexpr int32_t kShortStandard = 1;
static constexpr int32_t kShortDaylight = 3;

template <typename T>
static bool Bind(HMODULE module, const char* name, T* into) {
  *into = reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(module, name)));
  return *into != nullptr;
}

static const Icu* LoadIcu() {
  static const Icu* loaded = []() -> const Icu* {
    const HMODULE module = ::LoadLibraryExA("icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      return nullptr;
    }

    static Icu icu;
    const bool bound = Bind(module, "ucal_open", &icu.Open) && Bind(module, "ucal_setMillis", &icu.SetMillis) && Bind(module, "ucal_get", &icu.Get) && Bind(module, "ucal_inDaylightTime", &icu.InDaylight) && Bind(module, "ucal_getTimeZoneDisplayName", &icu.DisplayName) && Bind(module, "ucal_close", &icu.Close);
    return bound ? &icu : nullptr;
  }();
  return loaded;
}

static void* OpenCalendar(const Icu& icu, const std::string& zone, int64_t seconds) {
  const std::u16string wide(zone.begin(), zone.end());
  int32_t status = 0;
  void* calendar = icu.Open(wide.empty() ? nullptr : wide.data(), static_cast<int32_t>(wide.size()), "en_US", kGregorian, &status);
  if (calendar == nullptr || status > 0) {
    return nullptr;
  }

  icu.SetMillis(calendar, static_cast<double>(seconds) * 1000.0, &status);
  if (status > 0) {
    icu.Close(calendar);
    return nullptr;
  }
  return calendar;
}
#endif

extern "C" int32_t wolang_zone_offset(int64_t seconds, const char* zone,
                                      uint64_t length) {
  std::string named(zone != nullptr ? zone : "",
                    static_cast<size_t>(length));
#if defined(_WIN32)
  if (const Icu* icu = LoadIcu(); icu != nullptr) {
    if (void* calendar = OpenCalendar(*icu, named, seconds); calendar != nullptr) {
      int32_t status = 0;
      const int32_t offset = icu->Get(calendar, kZoneOffsetField, &status) + icu->Get(calendar, kDstOffsetField, &status);
      icu->Close(calendar);
      if (status <= 0) {
        return offset / 1000;
      }
    }
  }
#endif

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
#if defined(_WIN32)
  if (const Icu* icu = LoadIcu(); icu != nullptr) {
    if (void* calendar = OpenCalendar(*icu, named, seconds); calendar != nullptr) {
      int32_t status = 0;
      const bool daylight = icu->InDaylight(calendar, &status) != 0;
      char16_t wide[64] = {};
      const int32_t count = icu->DisplayName(calendar, daylight ? kShortDaylight : kShortStandard, "en_US", wide, 64, &status);
      icu->Close(calendar);
      if (status <= 0 && count > 0) {
        uint64_t size = 0;
        for (int32_t i = 0; i < count && size < room; ++i) {
          into[size++] = wide[i] < 0x80 ? static_cast<uint8_t>(wide[i]) : static_cast<uint8_t>('?');
        }
        return static_cast<int32_t>(size);
      }
    }
  }
#endif

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
