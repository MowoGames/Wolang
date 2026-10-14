// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/token_kind.h"

#include "harness/test.h"

namespace wolang {
namespace {

WOLANG_TEST(TokenKind, NamesAndSpellings) {
  WOLANG_EXPECT_EQ(TokenKindName(TokenKind::kKwFun), "KwFun");
  WOLANG_EXPECT_EQ(TokenKindSpelling(TokenKind::kKwFun), "fun");
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
  WOLANG_EXPECT_EQ(LookupKeyword("fun"), TokenKind::kKwFun);
  WOLANG_EXPECT_EQ(LookupKeyword("need"), TokenKind::kKwNeed);
  WOLANG_EXPECT_EQ(LookupKeyword("offer"), TokenKind::kKwOffer);
  WOLANG_EXPECT_EQ(LookupKeyword("foreach"), TokenKind::kKwForeach);
  WOLANG_EXPECT_EQ(LookupKeyword("uses"), TokenKind::kKwUses);
  WOLANG_EXPECT_EQ(LookupKeyword("arena"), TokenKind::kKwArena);
  WOLANG_EXPECT_EQ(LookupKeyword("const"), TokenKind::kKwConst);
  WOLANG_EXPECT_EQ(LookupKeyword("task"), TokenKind::kKwTask);
  WOLANG_EXPECT_EQ(LookupKeyword("wait"), TokenKind::kKwWait);
  // The words these replaced are ordinary identifiers again.
  WOLANG_EXPECT_EQ(LookupKeyword("let"), TokenKind::kIdentifier);
  WOLANG_EXPECT_EQ(LookupKeyword("async"), TokenKind::kIdentifier);
  WOLANG_EXPECT_EQ(LookupKeyword("await"), TokenKind::kIdentifier);
  WOLANG_EXPECT_EQ(LookupKeyword("region"), TokenKind::kIdentifier);
  // Keywords are case sensitive and not matched as prefixes.
  WOLANG_EXPECT_EQ(LookupKeyword("Fun"), TokenKind::kIdentifier);
  WOLANG_EXPECT_EQ(LookupKeyword("funny"), TokenKind::kIdentifier);
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
  // The spellings `fun`, `need` and `offer` replaced are reserved, so
  // old habits get a message rather than a confusing parse failure.
  WOLANG_EXPECT(IsReservedWord("fn"));
  WOLANG_EXPECT(IsReservedWord("import"));
  WOLANG_EXPECT(IsReservedWord("pub"));
  WOLANG_EXPECT(IsReservedWord("impl"));
  WOLANG_EXPECT(IsReservedWord("kernel"));
  WOLANG_EXPECT(IsReservedWord("yield"));
  WOLANG_EXPECT(!IsReservedWord("fun"));
  WOLANG_EXPECT(!IsReservedWord("health"));
  WOLANG_EXPECT_EQ(LookupKeyword("transactional"), TokenKind::kIdentifier);
}

}  // namespace
}  // namespace wolang
