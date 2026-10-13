// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_BASE_UNICODE_H_
#define WOLANG_BASE_UNICODE_H_

#include <cstdint>
#include <string_view>

namespace wolang {

// The Unicode facts the lexer needs. Tables are generated from the Unicode
// Character Database by tools/gen/gen_unicode_tables.py, so the compiler needs
// no Unicode library and moving to a new Unicode version is one command.

constexpr uint32_t kInvalidCodePoint = 0xFFFFFFFFu;

// Decodes the UTF-8 sequence starting at |offset|. Returns its length in
// bytes and stores the code point in |code_point|; returns 0 for malformed,
// overlong and surrogate sequences.
uint32_t DecodeUtf8(std::string_view text,
                    uint32_t offset,
                    uint32_t* code_point);

// Appends |code_point| to |out| as UTF-8. The caller must pass a scalar value.
void AppendUtf8(uint32_t code_point, std::string* out);

// XID_Start / XID_Continue. These are the Unicode-recommended identifier
// properties: letters and marks that make up words in real scripts, and
// nothing else -- no emoji, no symbols, no invisible formatting characters.
bool IsIdentifierStart(uint32_t code_point);
bool IsIdentifierContinue(uint32_t code_point);

// True when |text| is already in Unicode Normalization Form C.
//
// Wolang requires identifiers to be in NFC rather than normalizing them,
// which is what guarantees that one name has one spelling: a decomposed
// `й` (U+0438 U+0306) is rejected with a diagnostic instead of silently
// comparing equal to the precomposed one. The check is exact, not
// conservative -- a combining mark that cannot compose with what precedes it,
// as in many Indic and Vietnamese sequences, is accepted.
bool IsNormalizedNfc(std::string_view text);

// Canonical combining class, zero for the vast majority of code points.
uint8_t CombiningClass(uint32_t code_point);

}  // namespace wolang

#endif  // WOLANG_BASE_UNICODE_H_
