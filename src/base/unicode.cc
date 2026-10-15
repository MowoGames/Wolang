// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/base/unicode.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace wolang {
namespace {

struct CodePointRange {
  uint32_t begin;
  uint32_t end;
};

struct ClassRange {
  uint32_t begin;
  uint32_t end;
  uint8_t value;
};

struct CompositionPair {
  uint32_t first;
  uint32_t second;
  uint32_t composed;
};

// Counting the table entries with the same x-macro that fills them keeps the
// array sizes and the data from ever disagreeing.
#define WOLANG_XID_START_RANGE(begin, end) +1
constexpr size_t kXidStartCount = 0
#include "wolang/base/unicode_tables.inc"
    ;

#define WOLANG_XID_CONTINUE_RANGE(begin, end) +1
constexpr size_t kXidContinueCount = 0
#include "wolang/base/unicode_tables.inc"
    ;

#define WOLANG_NFC_NO_RANGE(begin, end) +1
constexpr size_t kNfcNoCount = 0
#include "wolang/base/unicode_tables.inc"
    ;

#define WOLANG_NFC_MAYBE_RANGE(begin, end) +1
constexpr size_t kNfcMaybeCount = 0
#include "wolang/base/unicode_tables.inc"
    ;

#define WOLANG_COMBINING_CLASS_RANGE(begin, end, value) +1
constexpr size_t kCombiningCount = 0
#include "wolang/base/unicode_tables.inc"
    ;

#define WOLANG_COMPOSITION_PAIR(first, second, composed) +1
constexpr size_t kCompositionCount = 0
#include "wolang/base/unicode_tables.inc"
    ;

constexpr std::array<CodePointRange, kXidStartCount> kXidStart = {{
#define WOLANG_XID_START_RANGE(begin, end) {begin, end},
#include "wolang/base/unicode_tables.inc"
}};

constexpr std::array<CodePointRange, kXidContinueCount> kXidContinue = {{
#define WOLANG_XID_CONTINUE_RANGE(begin, end) {begin, end},
#include "wolang/base/unicode_tables.inc"
}};

constexpr std::array<CodePointRange, kNfcNoCount> kNfcNo = {{
#define WOLANG_NFC_NO_RANGE(begin, end) {begin, end},
#include "wolang/base/unicode_tables.inc"
}};

constexpr std::array<CodePointRange, kNfcMaybeCount> kNfcMaybe = {{
#define WOLANG_NFC_MAYBE_RANGE(begin, end) {begin, end},
#include "wolang/base/unicode_tables.inc"
}};

constexpr std::array<ClassRange, kCombiningCount> kCombining = {{
#define WOLANG_COMBINING_CLASS_RANGE(begin, end, value) {begin, end, value},
#include "wolang/base/unicode_tables.inc"
}};

constexpr std::array<CompositionPair, kCompositionCount> kCompositions = {{
#define WOLANG_COMPOSITION_PAIR(first, second, composed) \
  {first, second, composed},
#include "wolang/base/unicode_tables.inc"
}};

template <size_t N>
bool InRanges(const std::array<CodePointRange, N>& ranges,
              uint32_t code_point) {
  // The generator emits ranges in order, so a binary search is valid.
  const auto it = std::upper_bound(
      ranges.begin(), ranges.end(), code_point,
      [](uint32_t value, const CodePointRange& range) {
        return value < range.begin;
      });
  if (it == ranges.begin()) {
    return false;
  }
  const CodePointRange& range = *(it - 1);
  return code_point <= range.end;
}

// Hangul syllables compose arithmetically, so they stay out of the pair table.
constexpr uint32_t kHangulLBase = 0x1100;
constexpr uint32_t kHangulVBase = 0x1161;
constexpr uint32_t kHangulTBase = 0x11A7;
constexpr uint32_t kHangulSBase = 0xAC00;
constexpr uint32_t kHangulLCount = 19;
constexpr uint32_t kHangulVCount = 21;
constexpr uint32_t kHangulTCount = 28;
constexpr uint32_t kHangulNCount = kHangulVCount * kHangulTCount;
constexpr uint32_t kHangulSCount = kHangulLCount * kHangulNCount;

// Returns the composition of |first| and |second|, or kInvalidCodePoint.
uint32_t Compose(uint32_t first, uint32_t second) {
  if (first >= kHangulLBase && first < kHangulLBase + kHangulLCount &&
      second >= kHangulVBase && second < kHangulVBase + kHangulVCount) {
    return kHangulSBase +
           ((first - kHangulLBase) * kHangulVCount +
            (second - kHangulVBase)) *
               kHangulTCount;
  }
  if (first >= kHangulSBase && first < kHangulSBase + kHangulSCount &&
      (first - kHangulSBase) % kHangulTCount == 0 &&
      second > kHangulTBase && second < kHangulTBase + kHangulTCount) {
    return first + (second - kHangulTBase);
  }

  const auto it = std::lower_bound(
      kCompositions.begin(), kCompositions.end(),
      CompositionPair{first, second, 0},
      [](const CompositionPair& lhs, const CompositionPair& rhs) {
        if (lhs.first != rhs.first) {
          return lhs.first < rhs.first;
        }
        return lhs.second < rhs.second;
      });
  if (it == kCompositions.end() || it->first != first ||
      it->second != second) {
    return kInvalidCodePoint;
  }
  return it->composed;
}

}  // namespace

uint32_t DecodeUtf8(std::string_view text,
                    uint32_t offset,
                    uint32_t* code_point) {
  if (offset >= text.size()) {
    return 0;
  }
  const auto first = static_cast<unsigned char>(text[offset]);
  uint32_t length = 0;
  uint32_t value = 0;
  if (first < 0x80) {
    length = 1;
    value = first;
  } else if ((first & 0xE0) == 0xC0) {
    length = 2;
    value = first & 0x1Fu;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    value = first & 0x0Fu;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    value = first & 0x07u;
  } else {
    return 0;
  }

  if (offset + length > text.size()) {
    return 0;
  }
  for (uint32_t i = 1; i < length; ++i) {
    const auto byte = static_cast<unsigned char>(text[offset + i]);
    if ((byte & 0xC0) != 0x80) {
      return 0;
    }
    value = (value << 6) | (byte & 0x3Fu);
  }

  // Reject overlong encodings, surrogates and out-of-range values, so that
  // everything downstream can assume well-formed UTF-8.
  static constexpr uint32_t kMinimum[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (value < kMinimum[length] || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return 0;
  }

  if (code_point != nullptr) {
    *code_point = value;
  }
  return length;
}

void AppendUtf8(uint32_t code_point, std::string* out) {
  if (code_point <= 0x7F) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

bool IsIdentifierStart(uint32_t code_point) {
  if (code_point < 0x80) {
    return code_point == '_' ||
           (code_point >= 'a' && code_point <= 'z') ||
           (code_point >= 'A' && code_point <= 'Z');
  }
  return InRanges(kXidStart, code_point);
}

bool IsIdentifierContinue(uint32_t code_point) {
  if (code_point < 0x80) {
    return IsIdentifierStart(code_point) ||
           (code_point >= '0' && code_point <= '9');
  }
  return InRanges(kXidContinue, code_point);
}

uint8_t CombiningClass(uint32_t code_point) {
  if (code_point < 0x300) {
    // No code point below U+0300 combines, which covers all ASCII text.
    return 0;
  }
  const auto it = std::upper_bound(
      kCombining.begin(), kCombining.end(), code_point,
      [](uint32_t value, const ClassRange& range) {
        return value < range.begin;
      });
  if (it == kCombining.begin()) {
    return 0;
  }
  const ClassRange& range = *(it - 1);
  return code_point <= range.end ? range.value : 0;
}

bool IsNormalizedNfc(std::string_view text) {
  uint32_t previous = kInvalidCodePoint;
  uint8_t previous_class = 0;

  uint32_t offset = 0;
  while (offset < text.size()) {
    uint32_t code_point = 0;
    const uint32_t length = DecodeUtf8(text, offset, &code_point);
    if (length == 0) {
      // Malformed UTF-8 is reported separately; nothing to say about its form.
      return true;
    }
    offset += length;

    const uint8_t code_point_class = CombiningClass(code_point);

    // A code point NFC rewrites on its own is never in normal form.
    if (InRanges(kNfcNo, code_point)) {
      return false;
    }

    // Hangul composes arithmetically, so its jamo are not in the pair table
    // and therefore not in the generated "maybe" set either.
    const bool hangul_maybe =
        (code_point >= kHangulVBase &&
         code_point < kHangulVBase + kHangulVCount) ||
        (code_point > kHangulTBase &&
         code_point < kHangulTBase + kHangulTCount);

    if ((hangul_maybe || InRanges(kNfcMaybe, code_point)) &&
        previous != kInvalidCodePoint) {
      // The pair only composes when nothing between them blocks it, which is
      // what the combining class comparison decides.
      const bool blocked =
          previous_class != 0 && previous_class >= code_point_class;
      if (!blocked && Compose(previous, code_point) != kInvalidCodePoint) {
        return false;
      }
    }

    // The starter a following mark would compose with stays the last code
    // point of class zero.
    if (code_point_class == 0) {
      previous = code_point;
    }
    previous_class = code_point_class;
  }
  return true;
}

}  // namespace wolang
