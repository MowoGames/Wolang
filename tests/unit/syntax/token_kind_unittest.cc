// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/token_kind.h"

#include "harness/test.h"

namespace wolang {
namespace {

WOLANG_TEST(TokenKind, NamesAndSpellings) {
  WOLANG_EXPECT_EQ(TokenKindName(TokenKind::kKwFn), "KwFn");
  WOLANG_EXPECT_EQ(TokenKindSpelling(TokenKind::kKwFn), "fn");
  WOLANG_EXPECT_EQ(TokenKindName(TokenKind::kLeftParen), "LeftParen");
  WOLANG_EXPECT_EQ(TokenKindSpelling(TokenKind::kLeftParen), "(");
  WOLANG_EXPECT_EQ(TokenKindSpelling(TokenKind::kIdentifier),
                   "<identifier>");
}

WOLANG_TEST(TokenKind, EveryKindHasANameAndSpelling) {
  for (uint16_t i = 0; i < static_cast<uint16_t>(TokenKind::kCount); ++i) {
    const auto kind = static_cast<TokenKind>(i);
    WOLANG_ASSERT(!TokenKindName(kind).empty());
    WOLANG_ASSERT(!TokenKindSpelling(kind).empty());
  }
}

WOLANG_TEST(TokenKind, ClassifiesCategories) {
  WOLANG_EXPECT(IsKeyword(TokenKind::kKwMatch));
  WOLANG_EXPECT(!IsKeyword(TokenKind::kIdentifier));
  WOLANG_EXPECT(!IsKeyword(TokenKind::kPlus));

  WOLANG_EXPECT(IsPunctuation(TokenKind::kPlus));
  WOLANG_EXPECT(IsPunctuation(TokenKind::kQuestionQuestionEq));
  WOLANG_EXPECT(!IsPunctuation(TokenKind::kKwMatch));
  WOLANG_EXPECT(!IsPunctuation(TokenKind::kIntLiteral));

  WOLANG_EXPECT(IsLiteral(TokenKind::kIntLiteral));
  WOLANG_EXPECT(IsLiteral(TokenKind::kKwTrue));
  WOLANG_EXPECT(IsLiteral(TokenKind::kKwNull));
  WOLANG_EXPECT(!IsLiteral(TokenKind::kIdentifier));
}

WOLANG_TEST(TokenKind, LookupKeywordRecognizesEveryKeyword) {
  WOLANG_EXPECT_EQ(LookupKeyword("fn"), TokenKind::kKwFn);
  WOLANG_EXPECT_EQ(LookupKeyword("foreach"), TokenKind::kKwForeach);
  WOLANG_EXPECT_EQ(LookupKeyword("uses"), TokenKind::kKwUses);
  WOLANG_EXPECT_EQ(LookupKeyword("region"), TokenKind::kKwRegion);
  // Keywords are case sensitive and not matched as prefixes.
  WOLANG_EXPECT_EQ(LookupKeyword("Fn"), TokenKind::kIdentifier);
  WOLANG_EXPECT_EQ(LookupKeyword("fna"), TokenKind::kIdentifier);
  WOLANG_EXPECT_EQ(LookupKeyword(""), TokenKind::kIdentifier);
}

WOLANG_TEST(TokenKind, DeclarationWordsStayContextual) {
  // These must remain usable as ordinary identifiers.
  for (std::string_view word :
       {"state", "machine", "unit", "dimension", "effect", "derived", "enter",
        "exit", "always"}) {
    WOLANG_ASSERT_EQ(LookupKeyword(word), TokenKind::kIdentifier);
    WOLANG_ASSERT_NE(LookupContextualKeyword(word), ContextualKeyword::kNone);
  }

  WOLANG_EXPECT_EQ(LookupContextualKeyword("state"),
                   ContextualKeyword::kState);
  WOLANG_EXPECT_EQ(LookupContextualKeyword("health"),
                   ContextualKeyword::kNone);
  WOLANG_EXPECT_EQ(ContextualKeywordSpelling(ContextualKeyword::kMachine),
                   "machine");
  WOLANG_EXPECT(ContextualKeywordSpelling(ContextualKeyword::kNone).empty());
}

WOLANG_TEST(TokenKind, ReservedWordsAreNotKeywords) {
  WOLANG_EXPECT(IsReservedWord("transactional"));
  WOLANG_EXPECT(IsReservedWord("kernel"));
  WOLANG_EXPECT(IsReservedWord("yield"));
  WOLANG_EXPECT(!IsReservedWord("fn"));
  WOLANG_EXPECT(!IsReservedWord("health"));
  WOLANG_EXPECT_EQ(LookupKeyword("transactional"), TokenKind::kIdentifier);
}

}  // namespace
}  // namespace wolang
