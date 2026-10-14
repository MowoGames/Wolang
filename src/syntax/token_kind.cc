// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/token_kind.h"

#include <array>
#include <unordered_map>

namespace wolang {
namespace {

constexpr auto kTokenCount = static_cast<size_t>(TokenKind::kCount);

constexpr std::array<std::string_view, kTokenCount> kNames = {
#define WOLANG_TOKEN(Name, Spelling) #Name,
#include "wolang/syntax/token_kind.inc"
};

constexpr std::array<std::string_view, kTokenCount> kSpellings = {
#define WOLANG_TOKEN(Name, Spelling) Spelling,
#include "wolang/syntax/token_kind.inc"
};

constexpr std::array<bool, kTokenCount> kIsKeywordTable = {
#define WOLANG_TOKEN(Name, Spelling) false,
#define WOLANG_PUNCT(Name, Spelling) false,
#define WOLANG_KEYWORD(Name, Spelling) true,
#include "wolang/syntax/token_kind.inc"
};

constexpr std::array<bool, kTokenCount> kIsPunctuationTable = {
#define WOLANG_TOKEN(Name, Spelling) false,
#define WOLANG_PUNCT(Name, Spelling) true,
#define WOLANG_KEYWORD(Name, Spelling) false,
#include "wolang/syntax/token_kind.inc"
};

const std::unordered_map<std::string_view, TokenKind>& KeywordMap() {
  static const auto* map = new std::unordered_map<std::string_view, TokenKind>{
#define WOLANG_KEYWORD(Name, Spelling) {Spelling, TokenKind::k##Name},
#include "wolang/syntax/token_kind.inc"
  };
  return *map;
}

const std::unordered_map<std::string_view, ContextualKeyword>&
ContextualKeywordMap() {
  static const auto* map =
      new std::unordered_map<std::string_view, ContextualKeyword>{
#define WOLANG_CONTEXTUAL(Name, Spelling) \
  {Spelling, ContextualKeyword::k##Name},
#include "wolang/syntax/token_kind.inc"
      };
  return *map;
}

constexpr std::array<std::string_view,
                     static_cast<size_t>(ContextualKeyword::kCount)>
    kContextualSpellings = {
        "",
#define WOLANG_CONTEXTUAL(Name, Spelling) Spelling,
#include "wolang/syntax/token_kind.inc"
};

bool InRange(TokenKind kind) {
  return static_cast<size_t>(kind) < kTokenCount;
}

}  // namespace

std::string_view TokenKindName(TokenKind kind) {
  if (!InRange(kind)) {
    return "<invalid>";
  }
  return kNames[static_cast<size_t>(kind)];
}

std::string_view TokenKindSpelling(TokenKind kind) {
  if (!InRange(kind)) {
    return "<invalid>";
  }
  return kSpellings[static_cast<size_t>(kind)];
}

bool IsKeyword(TokenKind kind) {
  return InRange(kind) && kIsKeywordTable[static_cast<size_t>(kind)];
}

bool CanBeName(TokenKind kind) {
  if (!IsKeyword(kind)) {
    return false;
  }
  switch (kind) {
    // These begin an expression, so a name is not what they could be.
    case TokenKind::kKwIf:
    case TokenKind::kKwMatch:
    case TokenKind::kKwLoop:
    case TokenKind::kKwWhile:
    case TokenKind::kKwFor:
    case TokenKind::kKwForeach:
    case TokenKind::kKwUnsafe:
    case TokenKind::kKwArena:
    case TokenKind::kKwSpawn:
    case TokenKind::kKwWait:
    case TokenKind::kKwSelf:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse:
    case TokenKind::kKwNull:
    case TokenKind::kKwComptime:
    case TokenKind::kKwReturn:
    case TokenKind::kKwBreak:
    case TokenKind::kKwContinue:
    case TokenKind::kKwDefer:
    // These join an expression to what follows it.
    case TokenKind::kKwAs:
    case TokenKind::kKwIn:
    case TokenKind::kKwElse:
    case TokenKind::kKwUses:
    case TokenKind::kKwWhere:
    case TokenKind::kKwMut:
      return false;
    default:
      return true;
  }
}

bool IsPunctuation(TokenKind kind) {
  return InRange(kind) && kIsPunctuationTable[static_cast<size_t>(kind)];
}

bool IsLiteral(TokenKind kind) {
  switch (kind) {
    case TokenKind::kIntLiteral:
    case TokenKind::kFloatLiteral:
    case TokenKind::kStringLiteral:
    case TokenKind::kCharLiteral:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse:
    case TokenKind::kKwNull:
      return true;
    default:
      return false;
  }
}

TokenKind LookupKeyword(std::string_view text) {
  const auto& map = KeywordMap();
  auto it = map.find(text);
  return it == map.end() ? TokenKind::kIdentifier : it->second;
}

ContextualKeyword LookupContextualKeyword(std::string_view text) {
  const auto& map = ContextualKeywordMap();
  auto it = map.find(text);
  return it == map.end() ? ContextualKeyword::kNone : it->second;
}

std::string_view ContextualKeywordSpelling(ContextualKeyword keyword) {
  const auto index = static_cast<size_t>(keyword);
  if (index >= kContextualSpellings.size()) {
    return "";
  }
  return kContextualSpellings[index];
}

bool IsReservedWord(std::string_view text) {
  static const auto* words = new std::unordered_map<std::string_view, bool>{
#define WOLANG_RESERVED(Spelling) {Spelling, true},
#include "wolang/syntax/token_kind.inc"
  };
  return words->find(text) != words->end();
}

Precedence InfixPrecedence(TokenKind kind) {
  switch (kind) {
    case TokenKind::kEq:
    case TokenKind::kPlusEq:
    case TokenKind::kMinusEq:
    case TokenKind::kStarEq:
    case TokenKind::kSlashEq:
    case TokenKind::kPercentEq:
    case TokenKind::kAmpEq:
    case TokenKind::kPipeEq:
    case TokenKind::kCaretEq:
    case TokenKind::kShlEq:
    case TokenKind::kShrEq:
    case TokenKind::kQuestionQuestionEq:
      return Precedence::kAssign;
    case TokenKind::kDotDot:
    case TokenKind::kDotDotEq:
      return Precedence::kRange;
    case TokenKind::kQuestionQuestion:
      return Precedence::kCoalesce;
    case TokenKind::kPipePipe:
      return Precedence::kOr;
    case TokenKind::kAmpAmp:
      return Precedence::kAnd;
    case TokenKind::kEqEq:
    case TokenKind::kBangEq:
    case TokenKind::kLess:
    case TokenKind::kLessEq:
    case TokenKind::kGreater:
    case TokenKind::kGreaterEq:
      return Precedence::kCompare;
    case TokenKind::kPipe:
      return Precedence::kBitOr;
    case TokenKind::kCaret:
      return Precedence::kBitXor;
    case TokenKind::kAmp:
      return Precedence::kBitAnd;
    case TokenKind::kShl:
    case TokenKind::kShr:
      return Precedence::kShift;
    case TokenKind::kPlus:
    case TokenKind::kMinus:
      return Precedence::kAdd;
    case TokenKind::kStar:
    case TokenKind::kSlash:
    case TokenKind::kPercent:
      return Precedence::kMultiply;
    case TokenKind::kKwAs:
      return Precedence::kCast;
    default:
      return Precedence::kNone;
  }
}

bool IsAssignmentOperator(TokenKind kind) {
  return InfixPrecedence(kind) == Precedence::kAssign;
}

}  // namespace wolang
