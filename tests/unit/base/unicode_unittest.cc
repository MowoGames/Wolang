// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/base/unicode.h"

#include <string>

#include "harness/test.h"

namespace wolang {
namespace {

std::string Utf8(uint32_t code_point) {
  std::string out;
  AppendUtf8(code_point, &out);
  return out;
}

std::string Utf8(std::initializer_list<uint32_t> code_points) {
  std::string out;
  for (uint32_t code_point : code_points) {
    AppendUtf8(code_point, &out);
  }
  return out;
}

WOLANG_TEST(Unicode, EncodesAndDecodesEveryLength) {
  for (uint32_t code_point : {uint32_t{'a'}, uint32_t{0x7F}, uint32_t{0x80},
                              uint32_t{0x7FF}, uint32_t{0x800},
                              uint32_t{0xFFFD}, uint32_t{0x10000},
                              uint32_t{0x10FFFF}}) {
    const std::string encoded = Utf8(code_point);
    uint32_t decoded = 0;
    const uint32_t length = DecodeUtf8(encoded, 0, &decoded);
    WOLANG_ASSERT_EQ(length, static_cast<uint32_t>(encoded.size()));
    WOLANG_EXPECT_EQ(decoded, code_point);
  }
}

WOLANG_TEST(Unicode, RejectsMalformedSequences) {
  // A continuation byte on its own.
  WOLANG_EXPECT_EQ(DecodeUtf8("\x80", 0, nullptr), 0u);
  // A truncated three-byte sequence.
  WOLANG_EXPECT_EQ(DecodeUtf8("\xE2\x82", 0, nullptr), 0u);
  // An overlong encoding of `/`.
  WOLANG_EXPECT_EQ(DecodeUtf8("\xC0\xAF", 0, nullptr), 0u);
  // A surrogate half, which UTF-8 may not encode.
  WOLANG_EXPECT_EQ(DecodeUtf8("\xED\xA0\x80", 0, nullptr), 0u);
  // Past the end of the input.
  WOLANG_EXPECT_EQ(DecodeUtf8("a", 1, nullptr), 0u);
}

WOLANG_TEST(Unicode, IdentifierStartFollowsXid) {
  WOLANG_EXPECT(IsIdentifierStart('a'));
  WOLANG_EXPECT(IsIdentifierStart('Z'));
  WOLANG_EXPECT(IsIdentifierStart('_'));
  WOLANG_EXPECT(!IsIdentifierStart('0'));
  WOLANG_EXPECT(!IsIdentifierStart(' '));

  // Letters from scripts gameplay teams actually use.
  WOLANG_EXPECT(IsIdentifierStart(0x0441));  // CYRILLIC SMALL LETTER ES
  WOLANG_EXPECT(IsIdentifierStart(0x00E9));  // LATIN SMALL LETTER E ACUTE
  WOLANG_EXPECT(IsIdentifierStart(0x4E2D));  // CJK
  WOLANG_EXPECT(IsIdentifierStart(0x3042));  // HIRAGANA A
  WOLANG_EXPECT(IsIdentifierStart(0x0915));  // DEVANAGARI KA

  // Not letters: a name made of these would be unreadable or invisible.
  WOLANG_EXPECT(!IsIdentifierStart(0x1F680));  // ROCKET
  WOLANG_EXPECT(!IsIdentifierStart(0x2764));   // HEAVY BLACK HEART
  WOLANG_EXPECT(!IsIdentifierStart(0x200B));   // ZERO WIDTH SPACE
  WOLANG_EXPECT(!IsIdentifierStart(0x202E));   // RIGHT-TO-LEFT OVERRIDE
  WOLANG_EXPECT(!IsIdentifierStart(0x00A0));   // NO-BREAK SPACE
  WOLANG_EXPECT(!IsIdentifierStart(0x2212));   // MINUS SIGN
}

WOLANG_TEST(Unicode, IdentifierContinueAddsDigitsAndMarks) {
  WOLANG_EXPECT(IsIdentifierContinue('0'));
  WOLANG_EXPECT(IsIdentifierContinue('a'));
  WOLANG_EXPECT(IsIdentifierContinue('_'));
  // A combining mark continues a name even though it cannot start one.
  WOLANG_EXPECT(IsIdentifierContinue(0x0301));  // COMBINING ACUTE ACCENT
  WOLANG_EXPECT(!IsIdentifierStart(0x0301));
  WOLANG_EXPECT(!IsIdentifierContinue(0x1F680));
}

WOLANG_TEST(Unicode, CombiningClassIsZeroForOrdinaryText) {
  WOLANG_EXPECT_EQ(CombiningClass('a'), 0);
  WOLANG_EXPECT_EQ(CombiningClass(0x0441), 0);
  WOLANG_EXPECT_EQ(CombiningClass(0x4E2D), 0);
  WOLANG_EXPECT_EQ(CombiningClass(0x0301), 230);  // COMBINING ACUTE ACCENT
  WOLANG_EXPECT_EQ(CombiningClass(0x0306), 230);  // COMBINING BREVE
}

WOLANG_TEST(Unicode, AsciiIsAlwaysNormalized) {
  WOLANG_EXPECT(IsNormalizedNfc(""));
  WOLANG_EXPECT(IsNormalizedNfc("plain_name_42"));
}

WOLANG_TEST(Unicode, AcceptsPrecomposedFormsAndRejectsDecomposedOnes) {
  // `й` precomposed is NFC; the same letter written as `и` plus a breve is
  // not, so only one spelling of the name is accepted.
  WOLANG_EXPECT(IsNormalizedNfc(Utf8(0x0439)));
  WOLANG_EXPECT(!IsNormalizedNfc(Utf8({0x0438, 0x0306})));

  // `é` the same way.
  WOLANG_EXPECT(IsNormalizedNfc(Utf8(0x00E9)));
  WOLANG_EXPECT(!IsNormalizedNfc(Utf8({0x0065, 0x0301})));

  // Inside a longer name.
  WOLANG_EXPECT(!IsNormalizedNfc("cafe" + Utf8(0x0301)));
  WOLANG_EXPECT(IsNormalizedNfc("caf" + Utf8(0x00E9)));
}

WOLANG_TEST(Unicode, AcceptsMarksThatCannotCompose) {
  // The check is exact rather than conservative: a combining mark that has no
  // precomposed form with what precedes it is already in NFC. Rejecting these
  // would make Devanagari and Vietnamese names unspellable.
  WOLANG_EXPECT(IsNormalizedNfc(Utf8({0x0915, 0x093E})));  // का
  WOLANG_EXPECT(IsNormalizedNfc(Utf8({0x0E01, 0x0E48})));  // ก่
  // A Cyrillic letter with an acute accent does not compose either.
  WOLANG_EXPECT(IsNormalizedNfc(Utf8({0x0444, 0x0301})));
}

WOLANG_TEST(Unicode, RejectsSingletonsThatNfcRewrites) {
  // U+212B ANGSTROM SIGN normalizes to U+00C5, so it is never NFC.
  WOLANG_EXPECT(!IsNormalizedNfc(Utf8(0x212B)));
  WOLANG_EXPECT(IsNormalizedNfc(Utf8(0x00C5)));
}

WOLANG_TEST(Unicode, HandlesHangulComposition) {
  // Conjoining jamo compose arithmetically, so the decomposed form is not NFC.
  WOLANG_EXPECT(!IsNormalizedNfc(Utf8({0x1100, 0x1161})));
  WOLANG_EXPECT(IsNormalizedNfc(Utf8(0xAC00)));
}

WOLANG_TEST(Unicode, MalformedInputIsNotJudged) {
  // Invalid UTF-8 has its own diagnostic; the normalization check stays quiet
  // rather than adding a second, confusing message.
  WOLANG_EXPECT(IsNormalizedNfc("\xFF\xFE"));
}

}  // namespace
}  // namespace wolang
