// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// What a pad reports, turned into the one layout `gfx` offers (D247).
//
// Header-only and free of system headers, so the Linux evdev byte format
// can be tested from a buffer on any machine: the unit tests link the
// compiler and not this library, and the parsing is the part that goes
// wrong, not the `read` around it.

#ifndef WOLANG_PLATFORM_GAMEPAD_DECODE_H_
#define WOLANG_PLATFORM_GAMEPAD_DECODE_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wolang {
namespace platform {

constexpr int32_t kMaxGamepads = 4;
constexpr int32_t kGamepadAxes = 6;

// The standard layout, Xbox-shaped because that is what every other pad
// is mapped onto. Numbers are the project's own (see platform.h).
enum GamepadButton : int32_t {
  kPadA = 0, kPadB, kPadX, kPadY, kPadLeftBumper, kPadRightBumper,
  kPadBack, kPadStart, kPadGuide, kPadLeftStick, kPadRightStick,
  kPadDpadUp, kPadDpadDown, kPadDpadLeft, kPadDpadRight,
  kPadButtonCount
};

// Sticks run -1 to 1 with up and left negative (a screen's convention,
// not a maths one); triggers run 0 to 1.
enum GamepadAxis : int32_t {
  kPadLeftX = 0, kPadLeftY, kPadRightX, kPadRightY, kPadLeftTrigger,
  kPadRightTrigger
};

struct PadState {
  uint32_t buttons = 0;
  float axes[kGamepadAxes] = {};
};

// Scales |value| from [min, max] to [-1, 1], or to [0, 1] when
// |unsigned_range|; anything out of range is clamped. A pad that reports
// no range gets zero, the safe answer.
inline float NormalizeAxis(int32_t value, int32_t min, int32_t max,
                           bool unsigned_range) {
  if (max <= min) {
    return 0.0f;
  }
  if (value < min) {
    value = min;
  }
  if (value > max) {
    value = max;
  }
  const double span = static_cast<double>(max) - min;
  const double unit = (static_cast<double>(value) - min) / span;
  return static_cast<float>(unsigned_range ? unit : unit * 2.0 - 1.0);
}

inline void SetButton(PadState* state, int32_t button, bool down) {
  if (button < 0 || button >= kPadButtonCount) {
    return;
  }
  if (down) {
    state->buttons |= 1u << button;
  } else {
    state->buttons &= ~(1u << button);
  }
}

// ----------------------------------------------------------- evdev ---
//
// linux/input-event-codes.h values, copied: they are kernel ABI and will
// not change, and a test that includes no system header builds anywhere.
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;

constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kAbsZ = 0x02;
constexpr uint16_t kAbsRx = 0x03;
constexpr uint16_t kAbsRy = 0x04;
constexpr uint16_t kAbsRz = 0x05;
constexpr uint16_t kAbsHat0X = 0x10;
constexpr uint16_t kAbsHat0Y = 0x11;
constexpr int32_t kAbsRanges = 0x12;

// The xpad layout: A, B, X, Y are 0x130, 0x131, 0x133, 0x134.
inline int32_t ButtonFromEvdev(uint16_t code) {
  switch (code) {
    case 0x130: return kPadA;
    case 0x131: return kPadB;
    case 0x133: return kPadX;
    case 0x134: return kPadY;
    case 0x136: return kPadLeftBumper;
    case 0x137: return kPadRightBumper;
    case 0x13a: return kPadBack;
    case 0x13b: return kPadStart;
    case 0x13c: return kPadGuide;
    case 0x13d: return kPadLeftStick;
    case 0x13e: return kPadRightStick;
    default: return -1;
  }
}

// The range a pad reported for each absolute axis (EVIOCGABS).
struct AbsRange {
  int32_t min = -32768;
  int32_t max = 32767;
};

// Applies one event to |state|. |ranges| is indexed by the evdev axis
// code and holds kAbsRanges entries. Answers whether it changed anything
// a program can see.
inline bool ApplyEvdev(PadState* state, const AbsRange* ranges,
                       uint16_t type, uint16_t code, int32_t value) {
  if (type == kEvKey) {
    const int32_t button = ButtonFromEvdev(code);
    if (button < 0) {
      return false;
    }
    SetButton(state, button, value != 0);
    return true;
  }
  if (type != kEvAbs) {
    return false;
  }
  switch (code) {
    case kAbsX:
    case kAbsY:
    case kAbsRx:
    case kAbsRy: {
      const int32_t axis = code == kAbsX    ? kPadLeftX
                           : code == kAbsY  ? kPadLeftY
                           : code == kAbsRx ? kPadRightX
                                            : kPadRightY;
      state->axes[axis] = NormalizeAxis(value, ranges[code].min,
                                        ranges[code].max, false);
      return true;
    }
    case kAbsZ:
    case kAbsRz: {
      const int32_t axis = code == kAbsZ ? kPadLeftTrigger
                                         : kPadRightTrigger;
      state->axes[axis] = NormalizeAxis(value, ranges[code].min,
                                        ranges[code].max, true);
      return true;
    }
    case kAbsHat0X:
      SetButton(state, kPadDpadLeft, value < 0);
      SetButton(state, kPadDpadRight, value > 0);
      return true;
    case kAbsHat0Y:
      SetButton(state, kPadDpadUp, value < 0);
      SetButton(state, kPadDpadDown, value > 0);
      return true;
    default:
      return false;
  }
}

// One `struct input_event`: a timeval of two longs, then u16 type, u16
// code, s32 value. Sized from `long` so the 32-bit layout parses too.
constexpr size_t EvdevRecordSize() { return 2 * sizeof(long) + 8; }

// Applies every whole record in |bytes| and answers how many bytes it
// used: a non-blocking read can cut a record, and the caller keeps the
// rest.
inline size_t ParseEvdev(const uint8_t* bytes, size_t size, PadState* state,
                         const AbsRange* ranges, bool* changed) {
  const size_t record = EvdevRecordSize();
  size_t used = 0;
  while (size - used >= record) {
    const uint8_t* at = bytes + used + 2 * sizeof(long);
    uint16_t type = 0;
    uint16_t code = 0;
    int32_t value = 0;
    std::memcpy(&type, at, 2);
    std::memcpy(&code, at + 2, 2);
    std::memcpy(&value, at + 4, 4);
    if (ApplyEvdev(state, ranges, type, code, value) && changed != nullptr) {
      *changed = true;
    }
    used += record;
  }
  return used;
}

}  // namespace platform
}  // namespace wolang

#endif  // WOLANG_PLATFORM_GAMEPAD_DECODE_H_
