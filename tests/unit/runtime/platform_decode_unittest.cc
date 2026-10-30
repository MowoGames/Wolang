// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The platform layer's byte-level decoders, tested from buffers so that
// no pad, display or desktop is needed (D247).

#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test.h"
#include "platform/gamepad_decode.h"
#include "platform/uri_list.h"

namespace {

using wolang::platform::AbsRange;
using wolang::platform::PadState;

void Record(std::vector<uint8_t>* into, uint16_t type, uint16_t code,
            int32_t value) {
  std::vector<uint8_t> bytes(wolang::platform::EvdevRecordSize(), 0);
  const size_t at = 2 * sizeof(long);
  std::memcpy(&bytes[at], &type, 2);
  std::memcpy(&bytes[at + 2], &code, 2);
  std::memcpy(&bytes[at + 4], &value, 4);
  into->insert(into->end(), bytes.begin(), bytes.end());
}

std::vector<AbsRange> Ranges() {
  std::vector<AbsRange> ranges(wolang::platform::kAbsRanges);
  ranges[wolang::platform::kAbsZ] = {0, 255};
  ranges[wolang::platform::kAbsRz] = {0, 255};
  return ranges;
}

WOLANG_TEST(PlatformDecode, NormalizesAxisRanges) {
  using wolang::platform::NormalizeAxis;
  WOLANG_EXPECT(NormalizeAxis(-32768, -32768, 32767, false) == -1.0f);
  WOLANG_EXPECT(NormalizeAxis(32767, -32768, 32767, false) == 1.0f);
  WOLANG_EXPECT(NormalizeAxis(255, 0, 255, true) == 1.0f);
  WOLANG_EXPECT(NormalizeAxis(0, 0, 255, true) == 0.0f);
  // Out of range clamps; an empty range is zero rather than a division.
  WOLANG_EXPECT(NormalizeAxis(9999, 0, 255, true) == 1.0f);
  WOLANG_EXPECT(NormalizeAxis(5, 3, 3, false) == 0.0f);
}

WOLANG_TEST(PlatformDecode, ParsesEvdevButtonsAndAxes) {
  std::vector<uint8_t> bytes;
  Record(&bytes, 1, 0x130, 1);      // A down
  Record(&bytes, 1, 0x133, 1);      // X down (not 0x132)
  Record(&bytes, 3, 0x00, 32767);   // left stick full right
  Record(&bytes, 3, 0x02, 255);     // left trigger full
  Record(&bytes, 3, 0x10, -1);      // dpad left
  Record(&bytes, 0, 0, 0);          // sync: nothing a program sees
  PadState state;
  const std::vector<AbsRange> ranges = Ranges();
  bool changed = false;
  const size_t used = wolang::platform::ParseEvdev(
      bytes.data(), bytes.size(), &state, ranges.data(), &changed);
  WOLANG_EXPECT(used == bytes.size());
  WOLANG_EXPECT(changed);
  WOLANG_EXPECT((state.buttons & (1u << wolang::platform::kPadA)) != 0);
  WOLANG_EXPECT((state.buttons & (1u << wolang::platform::kPadX)) != 0);
  WOLANG_EXPECT((state.buttons & (1u << wolang::platform::kPadB)) == 0);
  WOLANG_EXPECT((state.buttons & (1u << wolang::platform::kPadDpadLeft)) !=
                0);
  WOLANG_EXPECT(state.axes[wolang::platform::kPadLeftX] == 1.0f);
  WOLANG_EXPECT(state.axes[wolang::platform::kPadLeftTrigger] == 1.0f);

  // Releasing and centring the hat clears them.
  bytes.clear();
  Record(&bytes, 1, 0x130, 0);
  Record(&bytes, 3, 0x10, 0);
  wolang::platform::ParseEvdev(bytes.data(), bytes.size(), &state,
                               ranges.data(), nullptr);
  WOLANG_EXPECT((state.buttons & (1u << wolang::platform::kPadA)) == 0);
  WOLANG_EXPECT((state.buttons & (1u << wolang::platform::kPadDpadLeft)) ==
                0);
}

WOLANG_TEST(PlatformDecode, KeepsAPartialEvdevRecord) {
  std::vector<uint8_t> bytes;
  Record(&bytes, 1, 0x130, 1);
  const size_t whole = bytes.size();
  bytes.resize(whole + 5);  // the start of a second record
  PadState state;
  const std::vector<AbsRange> ranges = Ranges();
  const size_t used = wolang::platform::ParseEvdev(
      bytes.data(), bytes.size(), &state, ranges.data(), nullptr);
  WOLANG_EXPECT(used == whole);
}

WOLANG_TEST(PlatformDecode, IgnoresUnknownEvdevCodes) {
  std::vector<uint8_t> bytes;
  Record(&bytes, 1, 0x2ff, 1);  // not a gamepad button
  Record(&bytes, 3, 0x08, 7);   // not an axis we map
  PadState state;
  const std::vector<AbsRange> ranges = Ranges();
  bool changed = false;
  wolang::platform::ParseEvdev(bytes.data(), bytes.size(), &state,
                               ranges.data(), &changed);
  WOLANG_EXPECT(!changed);
  WOLANG_EXPECT(state.buttons == 0);
}

WOLANG_TEST(PlatformDecode, ParsesADroppedUriList) {
  const std::vector<std::string> paths =
      wolang::platform::PathsFromUriList(
          "# a comment\r\nfile:///home/a/my%20file.txt\r\n"
          "file://localhost/tmp/b\r\nhttp://example.com/x\r\n"
          "file://otherhost/c\r\n");
  WOLANG_ASSERT(paths.size() == 2);
  WOLANG_EXPECT(paths[0] == "/home/a/my file.txt");
  WOLANG_EXPECT(paths[1] == "/tmp/b");
  WOLANG_EXPECT(wolang::platform::PathsFromUriList("").empty());
}

}  // namespace
