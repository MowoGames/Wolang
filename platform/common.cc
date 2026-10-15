// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "platform/common.h"

#include <chrono>
#include <thread>

namespace wolang {
namespace platform {

double Monotonic() {
  static const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  const std::chrono::duration<double> since =
      std::chrono::steady_clock::now() - start;
  return since.count();
}

}  // namespace platform
}  // namespace wolang

extern "C" double wolang_platform_time(void) {
  return wolang::platform::Monotonic();
}

extern "C" void wolang_platform_sleep(double seconds) {
  if (seconds <= 0.0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}
