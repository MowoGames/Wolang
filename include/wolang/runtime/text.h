// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_TEXT_H_
#define WOLANG_RUNTIME_TEXT_H_

#include <cstddef>
#include <cstdint>

namespace wolang {
namespace runtime {

// A string, as every tier sees it: the bytes and how many there are.
//
// One representation, which is the point. The interpreter used to hold a
// C++ string and the compiled tier a pointer and a length, and the two
// could not hand a string to each other -- so a program that built one
// could only run in one tier.
struct Text {
  const uint8_t* data = nullptr;
  uint64_t length = 0;
};

// A float as text: the shortest digits that read back as the same number,
// and `nan` for every NaN whatever its sign bit (D226). One function so the
// interpreter, the runtime and every compiled tier print the same bytes.
// Returns the length written; |capacity| of 32 is always enough.
size_t FormatFloat(double value, char* buffer, size_t capacity);

// The same for a 32-bit float, which arrives widened to a double: the
// shortest digits for the float, not for the double it was widened to, so
// 0.1 stays `0.1` rather than becoming 0.10000000149011612 (D226).
size_t FormatFloat32(double value, char* buffer, size_t capacity);

// What an interpolated string is made of.
//
// These allocate, and nothing frees them yet: the collector is a later
// wave, and until then a string that is built stays built. Said out loud
// here rather than discovered later -- a server would notice.
extern "C" {

Text* wolang_text_from_int(int64_t value);
Text* wolang_text_from_uint(uint64_t value);
double wolang_fmod(double left, double right);
Text* wolang_text_from_float(double value);
Text* wolang_text_from_float32(double value);
Text* wolang_text_from_bool(bool value);
Text* wolang_text_concat(const uint8_t* left,
                         uint64_t left_length,
                         const uint8_t* right,
                         uint64_t right_length);

// Whether two strings hold the same bytes. A pair of pointers is not an
// answer to that question, which is why `==` on a string is a call.
// Gives back a string this allocated. A string an `arena` block handed
// out is left to the block.
void wolang_text_free(const uint8_t* data);

bool wolang_text_equal(const uint8_t* left,
                       uint64_t left_length,
                       const uint8_t* right,
                       uint64_t right_length);

}  // extern "C"

// Which of two strings comes first, by bytes: negative, zero or
// positive (D172).
extern "C" int32_t wolang_text_compare(const uint8_t* left,
                                       uint64_t left_length,
                                       const uint8_t* right,
                                       uint64_t right_length);

// Writes a `text` and a line break, where |message| is the address of
// one. What `Print` is for a file compiled on its own, which links no
// standard library to call (D161).
extern "C" void wolang_print(const void* message);

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_TEXT_H_
