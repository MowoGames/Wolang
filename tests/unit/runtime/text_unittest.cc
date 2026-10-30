// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/text.h"

#include <limits>
#include <string>

#include "harness/test.h"

namespace wolang {
namespace runtime {
namespace {

std::string Format(double value) {
  char buffer[32];
  return std::string(buffer, FormatFloat(value, buffer, sizeof(buffer)));
}

std::string Format32(double value) {
  char buffer[32];
  return std::string(buffer, FormatFloat32(value, buffer, sizeof(buffer)));
}

// NaN prints the same whatever its sign bit: 0/0 is negative on x64 (D226).
WOLANG_TEST(RuntimeText, NanHasOneSpelling) {
  const double positive = std::numeric_limits<double>::quiet_NaN();
  WOLANG_EXPECT_EQ(Format(positive), "nan");
  WOLANG_EXPECT_EQ(Format(-positive), "nan");
  WOLANG_EXPECT_EQ(Format32(-positive), "nan");
  WOLANG_EXPECT_EQ(Format(1.0), "1");
}

// The digits read back as the same number (D226).
WOLANG_TEST(RuntimeText, FloatsRoundTrip) {
  WOLANG_EXPECT_EQ(Format(1.0 / 3.0), "0.3333333333333333");
  WOLANG_EXPECT_EQ(Format(1e-320), "1e-320");
  WOLANG_EXPECT_EQ(Format(0.1), "0.1");
  WOLANG_EXPECT_EQ(Format32(static_cast<double>(0.1f)), "0.1");
  WOLANG_EXPECT_EQ(Format(std::numeric_limits<double>::infinity()), "inf");
}

}  // namespace
}  // namespace runtime
}  // namespace wolang
