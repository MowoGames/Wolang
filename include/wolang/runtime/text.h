// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_TEXT_H_
#define WOLANG_RUNTIME_TEXT_H_

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

// What an interpolated string is made of.
//
// These allocate, and nothing frees them yet: the collector is a later
// wave, and until then a string that is built stays built. Said out loud
// here rather than discovered later -- a server would notice.
extern "C" {

Text* wolang_text_from_int(int64_t value);
Text* wolang_text_from_float(double value);
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

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_TEXT_H_
