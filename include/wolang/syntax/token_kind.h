// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SYNTAX_TOKEN_KIND_H_
#define WOLANG_SYNTAX_TOKEN_KIND_H_

#include <cstdint>
#include <string_view>

namespace wolang {

enum class TokenKind : uint16_t {
#define WOLANG_TOKEN(Name, Spelling) k##Name,
#include "wolang/syntax/token_kind.inc"
  kCount,
};

// Contextual keywords are lexed as identifiers; the parser matches them by
// symbol in the positions where they introduce a declaration.
enum class ContextualKeyword : uint8_t {
  kNone = 0,
#define WOLANG_CONTEXTUAL(Name, Spelling) k##Name,
#include "wolang/syntax/token_kind.inc"
  kCount,
};

// Returns the enumerator name, for example "LeftParen" or "KwFn". Intended for
// debug output and test failures.
std::string_view TokenKindName(TokenKind kind);

// Returns how the token is written in source, or a bracketed placeholder for
// categories without a fixed spelling, for example "<identifier>".
std::string_view TokenKindSpelling(TokenKind kind);

bool IsKeyword(TokenKind kind);
bool IsPunctuation(TokenKind kind);
bool IsLiteral(TokenKind kind);

// Returns the keyword token for |text|, or TokenKind::kIdentifier when |text|
// is not a keyword.
TokenKind LookupKeyword(std::string_view text);

// Returns the contextual keyword for |text|, or ContextualKeyword::kNone.
ContextualKeyword LookupContextualKeyword(std::string_view text);

std::string_view ContextualKeywordSpelling(ContextualKeyword keyword);

// True for words that are not keywords yet but may not be used as
// identifiers, so that adding them later cannot break existing code.
bool IsReservedWord(std::string_view text);

}  // namespace wolang

#endif  // WOLANG_SYNTAX_TOKEN_KIND_H_
