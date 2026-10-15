// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/text.h"

#include "wolang/runtime/arena.h"

#include <cstddef>
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

void wolang_text_free(const uint8_t* data) {
  if (data == nullptr) {
    return;
  }
  // The pair and its bytes are one allocation, and `data` points just
  // past the pair -- so what was allocated is a header's worth before it
  // (D133).
  auto* text = reinterpret_cast<Text*>(
      const_cast<uint8_t*>(data) - sizeof(Text));
  if (wolang_arena_owns(text)) {
    // Storage an open `arena` block handed out is reclaimed by the block
    // (D113), and freeing it here would free a pointer the allocator
    // never gave anyone.
    return;
  }
  std::free(text);
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

extern "C" void wolang_print(const void* message) {
  // What `Print` is for a file compiled on its own.
  //
  // A single file links no standard library, so `Print` there is a
  // declaration with no body -- which the interpreter answers with an
  // intrinsic and the compiled tiers could not answer at all, so a
  // binary built from such a file trapped the moment it printed. One
  // symbol, and all three tiers do the same thing (D161).
  //
  // The argument is the address of the text, because that is how an
  // aggregate travels in this ABI: a pointer and a length, in that
  // order.
  if (message == nullptr) {
    std::fputc('\n', stdout);
    return;
  }
  const Text* held = static_cast<const Text*>(message);
  if (held->data != nullptr && held->length != 0) {
    std::fwrite(held->data, 1, static_cast<size_t>(held->length), stdout);
  }
  std::fputc('\n', stdout);
}

// Which of two strings comes first, by bytes.
//
// Negative, zero or positive, like every other comparison in C. The
// bytes rather than the characters: a UTF-8 string compares the same way
// by bytes as by code points, which is the one thing that encoding was
// designed to make true (D172).
extern "C" int32_t wolang_text_compare(const uint8_t* left,
                                       uint64_t left_length,
                                       const uint8_t* right,
                                       uint64_t right_length) {
  const uint64_t shared =
      left_length < right_length ? left_length : right_length;
  if (shared != 0 && left != nullptr && right != nullptr) {
    const int held =
        std::memcmp(left, right, static_cast<size_t>(shared));
    if (held != 0) {
      return held < 0 ? -1 : 1;
    }
  }
  if (left_length == right_length) {
    return 0;
  }
  return left_length < right_length ? -1 : 1;
}

}  // namespace runtime
}  // namespace wolang


