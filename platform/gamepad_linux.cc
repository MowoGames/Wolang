// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Gamepads on Linux, straight from /dev/input/event* (D247).
//
// No libudev: it is another library to find, and a program that wants a
// pad can afford to list one directory a second. Reads are non-blocking
// so that polling never waits on a pad that has nothing to say.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include "platform/common.h"
#include "platform/gamepad_decode.h"

namespace {

using wolang::platform::AbsRange;
using wolang::platform::PadState;

struct Slot {
  int fd = -1;
  std::string path;
  std::string name;
  PadState state;
  AbsRange ranges[wolang::platform::kAbsRanges];
  // A read can end in the middle of a record; the rest comes next time.
  std::vector<uint8_t> carry;
};

struct Pads {
  Slot slots[wolang::platform::kMaxGamepads];
  std::vector<WolangEvent> pending;
  double next_scan = 0.0;
};

Pads& Held() {
  static Pads pads;
  return pads;
}

bool HasBit(const uint8_t* bits, size_t size, int bit) {
  return static_cast<size_t>(bit / 8) < size &&
         (bits[bit / 8] & (1 << (bit % 8))) != 0;
}

void Close(Pads* pads, int32_t index) {
  Slot& slot = pads->slots[index];
  if (slot.fd >= 0) {
    ::close(slot.fd);
  }
  slot = Slot();
  WolangEvent made = wolang::platform::Blank(
      kWolangEventGamepadDisconnected);
  made.key = index;
  pads->pending.push_back(made);
}

// A pad is a device with the first gamepad button and a stick axis;
// keyboards, mice and the motion-sensor node many pads also expose fail
// one or the other.
void TryOpen(Pads* pads, const std::string& path) {
  for (const Slot& slot : pads->slots) {
    if (slot.fd >= 0 && slot.path == path) {
      return;
    }
  }
  int32_t free_slot = -1;
  for (int32_t i = 0; i < wolang::platform::kMaxGamepads; ++i) {
    if (pads->slots[i].fd < 0) {
      free_slot = i;
      break;
    }
  }
  if (free_slot < 0) {
    return;
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  uint8_t keys[KEY_MAX / 8 + 1] = {};
  uint8_t abs[ABS_MAX / 8 + 1] = {};
  if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
      ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0 ||
      !HasBit(keys, sizeof(keys), BTN_SOUTH) ||
      !HasBit(abs, sizeof(abs), ABS_X)) {
    ::close(fd);
    return;
  }
  Slot& slot = pads->slots[free_slot];
  slot = Slot();
  slot.fd = fd;
  slot.path = path;
  char name[128] = {};
  if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
    slot.name = name;
  }
  // Ranges and starting positions: a stick held off-centre at connection
  // never sends an event until it moves.
  for (int code = 0; code < wolang::platform::kAbsRanges; ++code) {
    if (!HasBit(abs, sizeof(abs), code)) {
      continue;
    }
    struct input_absinfo info;
    if (::ioctl(fd, EVIOCGABS(code), &info) < 0) {
      continue;
    }
    slot.ranges[code].min = info.minimum;
    slot.ranges[code].max = info.maximum;
    wolang::platform::ApplyEvdev(&slot.state, slot.ranges, EV_ABS,
                                 static_cast<uint16_t>(code), info.value);
  }
  WolangEvent made = wolang::platform::Blank(kWolangEventGamepadConnected);
  made.key = free_slot;
  pads->pending.push_back(made);
}

void Scan(Pads* pads) {
  DIR* dir = ::opendir("/dev/input");
  if (dir == nullptr) {
    return;
  }
  while (const dirent* entry = ::readdir(dir)) {
    if (std::strncmp(entry->d_name, "event", 5) == 0) {
      TryOpen(pads, std::string("/dev/input/") + entry->d_name);
    }
  }
  ::closedir(dir);
}

void Drain(Pads* pads, int32_t index) {
  Slot& slot = pads->slots[index];
  uint8_t buffer[1024];
  for (;;) {
    const ssize_t got = ::read(slot.fd, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        // ENODEV when unplugged, and nothing else is recoverable.
        Close(pads, index);
      }
      return;
    }
    if (got == 0) {
      Close(pads, index);
      return;
    }
    slot.carry.insert(slot.carry.end(), buffer, buffer + got);
    const size_t used = wolang::platform::ParseEvdev(
        slot.carry.data(), slot.carry.size(), &slot.state, slot.ranges,
        nullptr);
    slot.carry.erase(slot.carry.begin(), slot.carry.begin() + used);
  }
}

bool Valid(int32_t index) {
  return index >= 0 && index < wolang::platform::kMaxGamepads;
}

}  // namespace

extern "C" int32_t wolang_gamepad_poll(WolangEvent* out) {
  Pads& pads = Held();
  if (pads.pending.empty()) {
    const double now = wolang::platform::Monotonic();
    if (now >= pads.next_scan) {
      pads.next_scan = now + 1.0;
      Scan(&pads);
    }
    for (int32_t i = 0; i < wolang::platform::kMaxGamepads; ++i) {
      if (pads.slots[i].fd >= 0) {
        Drain(&pads, i);
      }
    }
  }
  if (pads.pending.empty() || out == nullptr) {
    return 0;
  }
  *out = pads.pending.front();
  pads.pending.erase(pads.pending.begin());
  return 1;
}

extern "C" int32_t wolang_gamepad_connected(int32_t index) {
  return Valid(index) && Held().slots[index].fd >= 0 ? 1 : 0;
}

extern "C" const char* wolang_gamepad_name(int32_t index) {
  return Valid(index) ? Held().slots[index].name.c_str() : "";
}

extern "C" int32_t wolang_gamepad_button(int32_t index, int32_t button) {
  if (!Valid(index) || button < 0 ||
      button >= wolang::platform::kPadButtonCount) {
    return 0;
  }
  return (Held().slots[index].state.buttons >> button) & 1u;
}

extern "C" double wolang_gamepad_axis(int32_t index, int32_t axis) {
  if (!Valid(index) || axis < 0 ||
      axis >= wolang::platform::kGamepadAxes) {
    return 0.0;
  }
  return Held().slots[index].state.axes[axis];
}
