// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

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

// Whether a keyword may stand where a name is wanted.
//
// Most keywords can only begin a declaration or a clause of one, so in a
// pattern, a parameter list or the middle of an expression there is
// nothing else they could be -- and a language that refuses `const after
// = x` or a field called `tag` makes its users rename their data. The
// exceptions are the keywords that begin an expression of their own
// (`if`, `match`, `self`, `true`, ...): those stay keywords everywhere.
bool CanBeName(TokenKind kind);
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

// Binding power of a binary operator, loosest first. Shared by the parser,
// which uses it to build the tree, and the formatter, which uses it to decide
// where parentheses are needed -- grouping parentheses are not stored in the
// tree, so they have to be derived from precedence alone.
enum class Precedence : int {
  kNone = 0,
  kAssign,     // = += -= ...
  kRange,      // .. ..=
  kCoalesce,   // ??
  kOr,         // ||
  kAnd,        // &&
  kCompare,    // == != < <= > >=
  kBitOr,      // |
  kBitXor,     // ^
  kBitAnd,     // &
  kShift,      // << >>
  kAdd,        // + -
  kMultiply,   // * / %
  kCast,       // as
  // Prefix and postfix operators bind tighter than any of the above.
  kUnary,
};

// Returns the precedence of |kind| as a binary operator, or Precedence::kNone
// when it is not one.
Precedence InfixPrecedence(TokenKind kind);

// True for `=` and the compound assignments, which are right associative.
bool IsAssignmentOperator(TokenKind kind);

}  // namespace wolang

#endif  // WOLANG_SYNTAX_TOKEN_KIND_H_
