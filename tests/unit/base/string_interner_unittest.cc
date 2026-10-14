// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/base/string_interner.h"

#include <string>
#include <unordered_set>

#include "harness/test.h"

namespace wolang {
namespace {

WOLANG_TEST(StringInterner, SameTextYieldsSameSymbol) {
  StringInterner interner;
  const Symbol first = interner.Intern("Turret");
  const Symbol second = interner.Intern("Turret");
  WOLANG_EXPECT_EQ(first, second);
  WOLANG_EXPECT_EQ(interner.size(), size_t{1});
}

WOLANG_TEST(StringInterner, DifferentTextYieldsDifferentSymbols) {
  StringInterner interner;
  WOLANG_EXPECT_NE(interner.Intern("health"), interner.Intern("Health"));
  WOLANG_EXPECT_EQ(interner.size(), size_t{2});
}

WOLANG_TEST(StringInterner, TextRoundTrips) {
  StringInterner interner;
  const Symbol symbol = interner.Intern("wait");
  WOLANG_EXPECT_EQ(interner.Text(symbol), "wait");
}

WOLANG_TEST(StringInterner, CopiesTextSoCallerBufferCanDie) {
  StringInterner interner;
  Symbol symbol;
  {
    std::string temporary = "temporary";
    symbol = interner.Intern(temporary);
    temporary = "clobbered";
  }
  WOLANG_EXPECT_EQ(interner.Text(symbol), "temporary");
}

WOLANG_TEST(StringInterner, InvalidSymbolHasNoText) {
  StringInterner interner;
  const Symbol symbol;
  WOLANG_EXPECT(!symbol.is_valid());
  WOLANG_EXPECT(interner.Text(symbol).empty());
}

WOLANG_TEST(StringInterner, LookupDoesNotIntern) {
  StringInterner interner;
  WOLANG_EXPECT(!interner.Lookup("missing").is_valid());
  WOLANG_EXPECT_EQ(interner.size(), size_t{0});

  const Symbol symbol = interner.Intern("present");
  WOLANG_EXPECT_EQ(interner.Lookup("present"), symbol);
}

WOLANG_TEST(StringInterner, HandlesEmptyAndUnicodeText) {
  StringInterner interner;
  const Symbol empty = interner.Intern("");
  const Symbol unicode = interner.Intern("скорость");
  WOLANG_EXPECT(interner.Text(empty).empty());
  WOLANG_EXPECT_EQ(interner.Text(unicode), "скорость");
  WOLANG_EXPECT_NE(empty, unicode);
}

WOLANG_TEST(StringInterner, SymbolIsUsableAsHashKey) {
  StringInterner interner;
  std::unordered_set<Symbol> symbols;
  symbols.insert(interner.Intern("a"));
  symbols.insert(interner.Intern("b"));
  symbols.insert(interner.Intern("a"));
  WOLANG_EXPECT_EQ(symbols.size(), size_t{2});
}

WOLANG_TEST(StringInterner, ManySymbolsStayStable) {
  StringInterner interner;
  std::vector<Symbol> symbols;
  for (int i = 0; i < 2000; ++i) {
    symbols.push_back(interner.Intern("name" + std::to_string(i)));
  }
  for (int i = 0; i < 2000; ++i) {
    WOLANG_ASSERT_EQ(interner.Text(symbols[static_cast<size_t>(i)]),
                     "name" + std::to_string(i));
  }
}

}  // namespace
}  // namespace wolang
