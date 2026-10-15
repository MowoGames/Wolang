// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_SYNTAX_TOKEN_H_
#define WOLANG_SYNTAX_TOKEN_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/token_kind.h"

namespace wolang {

enum class TokenFlags : uint16_t {
  kNone = 0,
  // The token is the first one on its source line. Wolang separates statements
  // by line breaks, so the parser reads this instead of semicolon tokens.
  kStartsLine = 1 << 0,
  // At least one blank line precedes the token. Only the formatter cares.
  kBlankLineBefore = 1 << 1,
  // The lexer reported a diagnostic while scanning this token; its payload is
  // a best-effort recovery value.
  kHasError = 1 << 2,
};

inline TokenFlags operator|(TokenFlags lhs, TokenFlags rhs) {
  return static_cast<TokenFlags>(static_cast<uint16_t>(lhs) |
                                 static_cast<uint16_t>(rhs));
}

inline TokenFlags& operator|=(TokenFlags& lhs, TokenFlags rhs) {
  lhs = lhs | rhs;
  return lhs;
}

inline bool HasFlag(TokenFlags flags, TokenFlags flag) {
  return (static_cast<uint16_t>(flags) & static_cast<uint16_t>(flag)) != 0;
}

enum class NumericBase : uint8_t {
  kDecimal,
  kHexadecimal,
  kOctal,
  kBinary,
};

// An explicit type suffix such as the `u8` of `255u8` or the `f64` of `2.0f64`.
enum class NumericSuffix : uint8_t {
  kNone,
  kI8,
  kI16,
  kI32,
  kI64,
  kU8,
  kU16,
  kU32,
  kU64,
  kF32,
  kF64,
};

std::string_view NumericSuffixName(NumericSuffix suffix);

// The decoded payload of an integer or float literal.
struct NumberLiteral {
  bool is_float = false;
  // Set for integer literals, and for float literals whose value fits.
  uint64_t integer_value = 0;
  double float_value = 0.0;
  NumericBase base = NumericBase::kDecimal;
  NumericSuffix suffix = NumericSuffix::kNone;
  // The unit written after the digits, as in `0.5s` or `90deg`. Units are user
  // declared, so the lexer only records the spelling and sema resolves it.
  Symbol unit;

  bool has_unit() const { return unit.is_valid(); }
};

enum class StringPartKind : uint8_t {
  // Literal text with escape sequences already decoded.
  kText,
  // An interpolated expression; |span| covers the source inside the braces and
  // is parsed on demand.
  kInterpolation,
};

struct StringPart {
  StringPartKind kind = StringPartKind::kText;
  std::string text;
  SourceSpan span;
};

struct StringLiteral {
  bool is_raw = false;
  std::vector<StringPart> parts;

  // True when the literal has no interpolation, so its value is known after
  // lexing.
  bool is_simple() const {
    return parts.empty() ||
           (parts.size() == 1 && parts.front().kind == StringPartKind::kText);
  }

  // Returns the text of a simple literal, or an empty view otherwise.
  std::string_view simple_text() const {
    if (parts.empty()) {
      return {};
    }
    return parts.front().kind == StringPartKind::kText
               ? std::string_view(parts.front().text)
               : std::string_view();
  }
};

// A lexed token. Payloads live in side tables of the TokenStream, so a token
// stays 16 bytes and a whole file of tokens stays cache friendly.
struct Token {
  TokenKind kind = TokenKind::kUnknown;
  TokenFlags flags = TokenFlags::kNone;
  uint32_t offset = 0;
  uint32_t length = 0;
  // Identifiers and keywords: the interned Symbol index.
  // Integer and float literals: index into TokenStream::numbers.
  // String literals: index into TokenStream::strings.
  // Character literals: the decoded Unicode code point.
  // Everything else: zero.
  uint32_t aux = 0;

  bool starts_line() const { return HasFlag(flags, TokenFlags::kStartsLine); }
  bool has_error() const { return HasFlag(flags, TokenFlags::kHasError); }
  uint32_t end_offset() const { return offset + length; }
};

static_assert(sizeof(Token) == 16, "Token is expected to stay 16 bytes");

// The result of lexing one file: the token vector plus the payload tables it
// indexes into.
struct TokenStream {
  FileId file;
  std::vector<Token> tokens;
  std::vector<NumberLiteral> numbers;
  std::vector<StringLiteral> strings;
  // Spans of non-documentation comments, in source order. Documentation
  // comments are tokens instead, because they attach to declarations.
  std::vector<SourceSpan> comments;

  size_t size() const { return tokens.size(); }
  bool empty() const { return tokens.empty(); }

  // Tokens always end with an end-of-file token, so indices past the end
  // return it instead of going out of bounds.
  const Token& At(size_t index) const {
    return tokens[index < tokens.size() ? index : tokens.size() - 1];
  }

  SourceSpan SpanOf(const Token& token) const {
    return SourceSpan{file, token.offset, token.offset + token.length};
  }

  Symbol IdentifierOf(const Token& token) const { return Symbol{token.aux}; }

  const NumberLiteral& NumberOf(const Token& token) const {
    return numbers[token.aux];
  }

  const StringLiteral& StringOf(const Token& token) const {
    return strings[token.aux];
  }

  uint32_t CharValueOf(const Token& token) const { return token.aux; }
};

}  // namespace wolang

#endif  // WOLANG_SYNTAX_TOKEN_H_
