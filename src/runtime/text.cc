// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/text.h"

#include "wolang/runtime/arena.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wolang {
namespace runtime {
namespace {

// One allocation for the pair and its bytes together, so that a string is
// one thing to free when there is something to free it.
Text* Allocate(const char* bytes, size_t length) {
  // From the innermost `arena` block when there is one: a string built
  // inside a block goes when the block does.
  auto* text = static_cast<Text*>(
      wolang_arena_alloc(sizeof(Text) + length + 1));
  if (text == nullptr) {
    return nullptr;
  }
  auto* storage = reinterpret_cast<uint8_t*>(text + 1);
  if (length != 0 && bytes != nullptr) {
    std::memcpy(storage, bytes, length);
  }
  storage[length] = 0;
  text->data = storage;
  text->length = length;
  return text;
}

}  // namespace

Text* wolang_text_from_int(int64_t value) {
  char buffer[24] = {};
  const int written = std::snprintf(buffer, sizeof(buffer), "%lld",
                                    static_cast<long long>(value));
  return Allocate(buffer, written > 0 ? static_cast<size_t>(written) : 0);
}

Text* wolang_text_from_float(double value) {
  // The shortest form that reads back as the same number, which is what a
  // printed float has to be for a test to be able to expect it.
  char buffer[32] = {};
  int written = std::snprintf(buffer, sizeof(buffer), "%g", value);
  if (written <= 0) {
    return Allocate("0", 1);
  }
  return Allocate(buffer, static_cast<size_t>(written));
}

Text* wolang_text_from_bool(bool value) {
  return value ? Allocate("true", 4) : Allocate("false", 5);
}

Text* wolang_text_concat(const uint8_t* left,
                         uint64_t left_length,
                         const uint8_t* right,
                         uint64_t right_length) {
  Text* text = Allocate(nullptr, left_length + right_length);
  if (text == nullptr) {
    return nullptr;
  }
  auto* storage = const_cast<uint8_t*>(text->data);
  if (left_length != 0 && left != nullptr) {
    std::memcpy(storage, left, left_length);
  }
  if (right_length != 0 && right != nullptr) {
    std::memcpy(storage + left_length, right, right_length);
  }
  return text;
}

bool wolang_text_equal(const uint8_t* left,
                       uint64_t left_length,
                       const uint8_t* right,
                       uint64_t right_length) {
  if (left_length != right_length) {
    return false;
  }
  if (left_length == 0) {
    return true;
  }
  if (left == nullptr || right == nullptr) {
    return false;
  }
  return std::memcmp(left, right, left_length) == 0;
}

}  // namespace runtime
}  // namespace wolang
