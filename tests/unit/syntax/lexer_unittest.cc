// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/lexer.h"

#include <cmath>
#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"

namespace wolang {
namespace {

// Lexes one snippet and exposes the parts tests assert on.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = sources_.AddVirtualFile("<test>", std::string(code));
    stream_ = Lex(sources_, file_, &interner_, &diagnostics_);
  }

  const TokenStream& stream() const { return stream_; }
  const DiagnosticEngine& diagnostics() const { return diagnostics_; }
  const StringInterner& interner() const { return interner_; }

  // Token count excluding the trailing end-of-file token.
  size_t count() const { return stream_.size() - 1; }

  const Token& At(size_t index) const { return stream_.At(index); }

  // Space separated token kind names, which makes failures readable.
  std::string Kinds() const {
    std::string result;
    for (size_t i = 0; i + 1 < stream_.size(); ++i) {
      if (!result.empty()) {
        result.push_back(' ');
      }
      result.append(TokenKindName(stream_.At(i).kind));
    }
    return result;
  }

  std::string_view TextAt(size_t index) const {
    return sources_.SpanText(stream_.SpanOf(stream_.At(index)));
  }

  std::string_view SpellingAt(size_t index) const {
    return interner_.Text(stream_.IdentifierOf(stream_.At(index)));
  }

  // Comma separated diagnostic codes in report order.
  std::string Codes() const {
    std::string result;
    for (const Diagnostic& diagnostic : diagnostics_.diagnostics()) {
      if (!result.empty()) {
        result.push_back(',');
      }
      result.append(diagnostic.code);
    }
    return result;
  }

  bool HasCode(std::string_view code) const {
    for (const Diagnostic& diagnostic : diagnostics_.diagnostics()) {
      if (diagnostic.code == code) {
        return true;
      }
    }
    return false;
  }

 private:
  SourceManager sources_;
  StringInterner interner_;
  DiagnosticEngine diagnostics_;
  TokenStream stream_;
  FileId file_;
};

bool NearlyEqual(double actual, double expected) {
  return std::fabs(actual - expected) <= 1e-12 * std::fabs(expected) + 1e-15;
}

// ------------------------------------------------------------- structure ----

WOLANG_TEST(Lexer, EmptyInputYieldsOnlyEndOfFile) {
  Fixture fixture("");
  WOLANG_ASSERT_EQ(fixture.stream().size(), size_t{1});
  WOLANG_EXPECT_EQ(fixture.At(0).kind, TokenKind::kEndOfFile);
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, StreamAlwaysEndsWithEndOfFile) {
  Fixture fixture("fun f() {}\n");
  WOLANG_EXPECT_EQ(fixture.stream().tokens.back().kind,
                   TokenKind::kEndOfFile);
  WOLANG_EXPECT_EQ(fixture.stream().tokens.back().offset, 11u);
}

WOLANG_TEST(Lexer, IndexingPastTheEndReturnsEndOfFile) {
  Fixture fixture("fun");
  WOLANG_EXPECT_EQ(fixture.stream().At(999).kind, TokenKind::kEndOfFile);
}

WOLANG_TEST(Lexer, TokensCarrySpans) {
  Fixture fixture("const speed = 10.0");
  WOLANG_EXPECT_EQ(fixture.TextAt(0), "const");
  WOLANG_EXPECT_EQ(fixture.TextAt(1), "speed");
  WOLANG_EXPECT_EQ(fixture.TextAt(2), "=");
  WOLANG_EXPECT_EQ(fixture.TextAt(3), "10.0");
  WOLANG_EXPECT_EQ(fixture.At(1).offset, 6u);
  WOLANG_EXPECT_EQ(fixture.At(1).length, 5u);
}

// ------------------------------------------------- identifiers & keywords ---

WOLANG_TEST(Lexer, LexesNewKeywords) {
  Fixture fixture("extend Barrel as Damageable {}\nerror NotFound");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwExtend Identifier KwAs Identifier LeftBrace RightBrace "
                   "KwError Identifier");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, LexesDeclarations) {
  Fixture fixture("offer task fun Fire(target: Pawn) -> bool { return true }");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwOffer KwTask KwFun Identifier LeftParen Identifier Colon "
                   "Identifier RightParen Arrow Identifier LeftBrace "
                   "KwReturn KwTrue RightBrace");
}

WOLANG_TEST(Lexer, InternsIdentifierSpellings) {
  Fixture fixture("health Health health");
  WOLANG_EXPECT_EQ(fixture.SpellingAt(0), "health");
  WOLANG_EXPECT_EQ(fixture.SpellingAt(1), "Health");
  WOLANG_EXPECT_EQ(fixture.At(0).aux, fixture.At(2).aux);
  WOLANG_EXPECT_NE(fixture.At(0).aux, fixture.At(1).aux);
}

WOLANG_TEST(Lexer, ContextualKeywordsLexAsIdentifiers) {
  Fixture fixture("var state = 1");
  WOLANG_EXPECT_EQ(fixture.Kinds(), "KwVar Identifier Eq IntLiteral");
  WOLANG_EXPECT_EQ(fixture.SpellingAt(1), "state");
}

WOLANG_TEST(Lexer, UnderscoreIsItsOwnToken) {
  Fixture fixture("const _ = f(_x)");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwConst Underscore Eq Identifier LeftParen Identifier "
                   "RightParen");
}

WOLANG_TEST(Lexer, AcceptsUnicodeIdentifiers) {
  Fixture fixture("const скорость = 1");
  WOLANG_EXPECT_EQ(fixture.Kinds(), "KwConst Identifier Eq IntLiteral");
  WOLANG_EXPECT_EQ(fixture.SpellingAt(1), "скорость");
  WOLANG_EXPECT(fixture.Codes().empty());

  // Letters from any script are fine, as are digits and marks after the
  // first character.
  WOLANG_EXPECT(Fixture("const 速度 = 1").Codes().empty());
  WOLANG_EXPECT(Fixture("const café2 = 1").Codes().empty());
}

WOLANG_TEST(Lexer, RejectsNonLettersInIdentifiers) {
  // An emoji or a symbol would let two different names look identical, and
  // an invisible character would hide one name inside another.
  WOLANG_EXPECT_EQ(Fixture("const 🚀 = 1").Codes(), "E0013");
  WOLANG_EXPECT_EQ(Fixture("const a\u200Bb = 1").Codes(), "E0013");
  WOLANG_EXPECT_EQ(Fixture("const ❤ = 1").Codes(), "E0013");
}

WOLANG_TEST(Lexer, RequiresIdentifiersInNfcForm) {
  // `й` precomposed is accepted; the same letter spelled as `и` plus a
  // combining breve is rejected, so one name has exactly one spelling.
  WOLANG_EXPECT(Fixture("const й = 1").Codes().empty());
  WOLANG_EXPECT_EQ(Fixture("const и\u0306 = 1").Codes(), "E0014");
  WOLANG_EXPECT_EQ(Fixture("const cafe\u0301 = 1").Codes(), "E0014");

  // A mark that cannot compose with what precedes it is already in NFC.
  WOLANG_EXPECT(Fixture("const \u0915\u093E = 1").Codes().empty());
}

WOLANG_TEST(Lexer, SkipsBlockComments) {
  Fixture fixture("fun /* inline */ f() {}");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwFun Identifier LeftParen RightParen LeftBrace "
                   "RightBrace");
  WOLANG_EXPECT_EQ(fixture.stream().comments.size(), size_t{1});
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, BlockCommentsNest) {
  // Nesting is the only reason to reach for a block comment: commenting out a
  // region that already contains one has to work.
  Fixture fixture("fun /* outer /* inner */ still */ f() {}");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwFun Identifier LeftParen RightParen LeftBrace "
                   "RightBrace");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, BlockCommentsMaySpanLines) {
  Fixture fixture("fun f() {\n/* one\n   two */\n}");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwFun Identifier LeftParen RightParen LeftBrace "
                   "RightBrace");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, ReportsUnterminatedBlockComments) {
  WOLANG_EXPECT_EQ(Fixture("fun f() /* never closed").Codes(), "E0012");
  // An unclosed inner comment leaves the outer one open too.
  WOLANG_EXPECT_EQ(Fixture("/* outer /* inner */").Codes(), "E0012");
}

WOLANG_TEST(Lexer, ReportsReservedWords) {
  // `impl` became reserved when `extend` replaced it, so old habits get a
  // message instead of a confusing parse failure.
  WOLANG_EXPECT_EQ(Fixture("impl Vec3 {}").Codes(), "E0011");

  Fixture fixture("const transactional = 1");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0011");
  // Recovery keeps it an identifier so the rest of the line still parses.
  WOLANG_EXPECT_EQ(fixture.At(1).kind, TokenKind::kIdentifier);
  WOLANG_EXPECT(fixture.At(1).has_error());
}

// ------------------------------------------------------------- operators ----

WOLANG_TEST(Lexer, PrefersLongestOperator) {
  // The backslash only hides a C++ trigraph; the lexed text is "a ??= ...".
  Fixture fixture("a ?\?= b ?? c?.d");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "Identifier QuestionQuestionEq Identifier "
                   "QuestionQuestion Identifier QuestionDot Identifier");
}

WOLANG_TEST(Lexer, LexesShiftsAndComparisons) {
  Fixture fixture("a <<= b >>= c << d >> e <= f >= g");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "Identifier ShlEq Identifier ShrEq Identifier Shl "
                   "Identifier Shr Identifier LessEq Identifier GreaterEq "
                   "Identifier");
}

WOLANG_TEST(Lexer, LexesRangesAndArrows) {
  Fixture fixture("0..10 1..=2 -> => .");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "IntLiteral DotDot IntLiteral IntLiteral DotDotEq "
                   "IntLiteral Arrow FatArrow Dot");
}

WOLANG_TEST(Lexer, LexesCompoundAssignments) {
  Fixture fixture("a += b -= c *= d /= e %= f &= g |= h ^= i");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "Identifier PlusEq Identifier MinusEq Identifier StarEq "
                   "Identifier SlashEq Identifier PercentEq Identifier AmpEq "
                   "Identifier PipeEq Identifier CaretEq Identifier");
}

WOLANG_TEST(Lexer, LexesGenericBracketsAsComparisons) {
  // The parser decides between generics and comparison; the lexer does not.
  Fixture fixture("Array<int>");
  WOLANG_EXPECT_EQ(fixture.Kinds(), "Identifier Less Identifier Greater");
}

WOLANG_TEST(Lexer, LexesAttributeSyntax) {
  Fixture fixture("@Range(1, 50)");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "At Identifier LeftParen IntLiteral Comma IntLiteral "
                   "RightParen");
}

WOLANG_TEST(Lexer, ReportsUnexpectedCharacters) {
  Fixture fixture("a $ b");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0005");
  WOLANG_EXPECT_EQ(fixture.Kinds(), "Identifier Unknown Identifier");
}

// --------------------------------------------------------------- numbers ----

WOLANG_TEST(Lexer, LexesDecimalIntegers) {
  Fixture fixture("0 7 1_000_000");
  WOLANG_EXPECT_EQ(fixture.Kinds(), "IntLiteral IntLiteral IntLiteral");
  WOLANG_EXPECT_EQ(fixture.stream().NumberOf(fixture.At(0)).integer_value,
                   uint64_t{0});
  WOLANG_EXPECT_EQ(fixture.stream().NumberOf(fixture.At(2)).integer_value,
                   uint64_t{1000000});
}

WOLANG_TEST(Lexer, LexesOtherBases) {
  Fixture fixture("0xFF 0b1010 0o17 0XfF");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "IntLiteral IntLiteral IntLiteral IntLiteral");
  const TokenStream& stream = fixture.stream();
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(0)).integer_value,
                   uint64_t{255});
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(0)).base,
                   NumericBase::kHexadecimal);
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(1)).integer_value,
                   uint64_t{10});
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(1)).base, NumericBase::kBinary);
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(2)).integer_value,
                   uint64_t{15});
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(2)).base, NumericBase::kOctal);
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(3)).integer_value,
                   uint64_t{255});
}

WOLANG_TEST(Lexer, LexesFloats) {
  Fixture fixture("3.14 1e3 1.5e-3 2E+2");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "FloatLiteral FloatLiteral FloatLiteral FloatLiteral");
  const TokenStream& stream = fixture.stream();
  WOLANG_EXPECT(NearlyEqual(stream.NumberOf(fixture.At(0)).float_value, 3.14));
  WOLANG_EXPECT(NearlyEqual(stream.NumberOf(fixture.At(1)).float_value, 1e3));
  WOLANG_EXPECT(
      NearlyEqual(stream.NumberOf(fixture.At(2)).float_value, 1.5e-3));
  WOLANG_EXPECT(NearlyEqual(stream.NumberOf(fixture.At(3)).float_value, 200.0));
}

WOLANG_TEST(Lexer, DotAfterIntegerStaysAnOperator) {
  Fixture fixture("1.max() 0..10");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "IntLiteral Dot Identifier LeftParen RightParen "
                   "IntLiteral DotDot IntLiteral");
}

WOLANG_TEST(Lexer, LexesTypeSuffixes) {
  Fixture fixture("255u8 -1s64 2.0f64 1f32");
  const TokenStream& stream = fixture.stream();
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(0)).suffix, NumericSuffix::kU8);
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(2)).suffix, NumericSuffix::kI64);
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(3)).suffix, NumericSuffix::kF64);
  // An `f32` suffix makes an otherwise integral literal a float.
  WOLANG_EXPECT_EQ(fixture.At(4).kind, TokenKind::kFloatLiteral);
  WOLANG_EXPECT_EQ(stream.NumberOf(fixture.At(4)).suffix, NumericSuffix::kF32);
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, LexesUnitSuffixesAsSymbols) {
  Fixture fixture("0.5s 200ms 90deg 10m 1.5rad");
  const TokenStream& stream = fixture.stream();
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.interner().Text(stream.NumberOf(fixture.At(0)).unit),
                   "s");
  WOLANG_EXPECT_EQ(fixture.interner().Text(stream.NumberOf(fixture.At(1)).unit),
                   "ms");
  WOLANG_EXPECT_EQ(fixture.interner().Text(stream.NumberOf(fixture.At(2)).unit),
                   "deg");
  WOLANG_EXPECT_EQ(fixture.interner().Text(stream.NumberOf(fixture.At(3)).unit),
                   "m");
  WOLANG_EXPECT_EQ(fixture.interner().Text(stream.NumberOf(fixture.At(4)).unit),
                   "rad");
  // Units keep the literal's own integer/float nature.
  WOLANG_EXPECT_EQ(fixture.At(1).kind, TokenKind::kIntLiteral);
  WOLANG_EXPECT_EQ(fixture.At(0).kind, TokenKind::kFloatLiteral);
  WOLANG_EXPECT(stream.NumberOf(fixture.At(0)).has_unit());
}

WOLANG_TEST(Lexer, ReportsIntegerOverflow) {
  Fixture fixture("18446744073709551616");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0008");
  WOLANG_EXPECT(fixture.At(0).has_error());
}

WOLANG_TEST(Lexer, ReportsMalformedNumbers) {
  WOLANG_EXPECT_EQ(Fixture("0x").Codes(), "E0004");
  WOLANG_EXPECT_EQ(Fixture("0b").Codes(), "E0004");
  WOLANG_EXPECT_EQ(Fixture("0b1012").Codes(), "E0004");
  WOLANG_EXPECT_EQ(Fixture("0o18").Codes(), "E0004");
}

WOLANG_TEST(Lexer, ReportsBadNumericSuffixes) {
  WOLANG_EXPECT_EQ(Fixture("1f128").Codes(), "E0010");
  WOLANG_EXPECT_EQ(Fixture("1i7").Codes(), "E0010");
  WOLANG_EXPECT_EQ(Fixture("1.5u8").Codes(), "E0010");
  // Units are only meaningful on decimal literals.
  WOLANG_EXPECT_EQ(Fixture("0xFFm").Codes(), "E0010");
}

// --------------------------------------------------------------- strings ----

WOLANG_TEST(Lexer, LexesSimpleStrings) {
  Fixture fixture(R"("hello")");
  WOLANG_ASSERT_EQ(fixture.At(0).kind, TokenKind::kStringLiteral);
  const StringLiteral& literal = fixture.stream().StringOf(fixture.At(0));
  WOLANG_EXPECT(literal.is_simple());
  WOLANG_EXPECT_EQ(literal.simple_text(), "hello");
}

WOLANG_TEST(Lexer, LexesEmptyString) {
  Fixture fixture(R"("")");
  const StringLiteral& literal = fixture.stream().StringOf(fixture.At(0));
  WOLANG_EXPECT(literal.parts.empty());
  WOLANG_EXPECT(literal.is_simple());
  WOLANG_EXPECT(literal.simple_text().empty());
}

WOLANG_TEST(Lexer, DecodesEscapeSequences) {
  Fixture fixture(R"("a\nb\t\\\"\{\u{41}\u{1F600}")");
  WOLANG_EXPECT(fixture.Codes().empty());
  const StringLiteral& literal = fixture.stream().StringOf(fixture.At(0));
  WOLANG_EXPECT_EQ(literal.simple_text(), "a\nb\t\\\"{A\xF0\x9F\x98\x80");
}

WOLANG_TEST(Lexer, ReportsUnknownEscapes) {
  Fixture fixture(R"("a\qb")");
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0002");
  // Recovery keeps the escaped character so the rest of the literal survives.
  WOLANG_EXPECT_EQ(fixture.stream().StringOf(fixture.At(0)).simple_text(),
                   "aqb");
}

WOLANG_TEST(Lexer, ReportsInvalidUnicodeEscapes) {
  WOLANG_EXPECT_EQ(Fixture(R"("\u41")").Codes(), "E0009");
  WOLANG_EXPECT_EQ(Fixture(R"("\u{}")").Codes(), "E0009");
  WOLANG_EXPECT_EQ(Fixture(R"("\u{110000}")").Codes(), "E0009");
  WOLANG_EXPECT_EQ(Fixture(R"("\u{D800}")").Codes(), "E0009");
}

WOLANG_TEST(Lexer, LexesRawStrings) {
  Fixture fixture(R"(r"a\nb{x}")");
  WOLANG_ASSERT_EQ(fixture.At(0).kind, TokenKind::kStringLiteral);
  const StringLiteral& literal = fixture.stream().StringOf(fixture.At(0));
  WOLANG_EXPECT(literal.is_raw);
  WOLANG_EXPECT_EQ(literal.simple_text(), "a\\nb{x}");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, SplitsInterpolatedStrings) {
  Fixture fixture(R"("hp {health} / {max}!")");
  WOLANG_EXPECT(fixture.Codes().empty());
  const StringLiteral& literal = fixture.stream().StringOf(fixture.At(0));
  WOLANG_ASSERT_EQ(literal.parts.size(), size_t{5});
  WOLANG_EXPECT(!literal.is_simple());

  WOLANG_EXPECT_EQ(literal.parts[0].kind, StringPartKind::kText);
  WOLANG_EXPECT_EQ(literal.parts[0].text, "hp ");
  WOLANG_EXPECT_EQ(literal.parts[1].kind, StringPartKind::kInterpolation);
  WOLANG_EXPECT_EQ(literal.parts[1].span.begin, 5u);
  WOLANG_EXPECT_EQ(literal.parts[1].span.end, 11u);
  WOLANG_EXPECT_EQ(literal.parts[2].text, " / ");
  WOLANG_EXPECT_EQ(literal.parts[3].kind, StringPartKind::kInterpolation);
  WOLANG_EXPECT_EQ(literal.parts[4].text, "!");
}

WOLANG_TEST(Lexer, HandlesNestedBracesAndQuotesInInterpolation) {
  Fixture fixture(R"("{ f(Map{a: "}"}) }")");
  WOLANG_EXPECT(fixture.Codes().empty());
  const StringLiteral& literal = fixture.stream().StringOf(fixture.At(0));
  WOLANG_ASSERT_EQ(literal.parts.size(), size_t{1});
  WOLANG_EXPECT_EQ(literal.parts[0].kind, StringPartKind::kInterpolation);
}

WOLANG_TEST(Lexer, ReportsEmptyInterpolation) {
  Fixture fixture(R"("{}")");
  WOLANG_EXPECT(fixture.HasCode("E0007"));
}

WOLANG_TEST(Lexer, ReportsUnterminatedInterpolation) {
  Fixture fixture(R"("{health")");
  WOLANG_EXPECT(fixture.HasCode("E0007"));
}

WOLANG_TEST(Lexer, ReportsUnterminatedStrings) {
  WOLANG_EXPECT(Fixture(R"("abc)").HasCode("E0001"));
  WOLANG_EXPECT(Fixture("\"abc\ndef\"").HasCode("E0001"));
  WOLANG_EXPECT(Fixture(R"(r"abc)").HasCode("E0001"));
}

WOLANG_TEST(Lexer, StringsMayNotSpanLinesButRawStringsMay) {
  Fixture fixture("r\"line1\nline2\"");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_EQ(fixture.stream().StringOf(fixture.At(0)).simple_text(),
                   "line1\nline2");
}

// ------------------------------------------------------------ characters ----

WOLANG_TEST(Lexer, LexesCharacterLiterals) {
  Fixture fixture("'a' '\\n' 'é'");
  WOLANG_EXPECT_EQ(fixture.Kinds(), "CharLiteral CharLiteral CharLiteral");
  WOLANG_EXPECT_EQ(fixture.stream().CharValueOf(fixture.At(0)), uint32_t{'a'});
  WOLANG_EXPECT_EQ(fixture.stream().CharValueOf(fixture.At(1)), uint32_t{10});
  WOLANG_EXPECT_EQ(fixture.stream().CharValueOf(fixture.At(2)),
                   uint32_t{0xE9});
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, ReportsBadCharacterLiterals) {
  WOLANG_EXPECT(Fixture("''").HasCode("E0003"));
  WOLANG_EXPECT(Fixture("'ab'").HasCode("E0003"));
  WOLANG_EXPECT(Fixture("'a").HasCode("E0003"));
}

// --------------------------------------------------------------- trivia -----

WOLANG_TEST(Lexer, SkipsLineCommentsAndRecordsThem) {
  Fixture fixture("// leading\nfun f() {} // trailing\n");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwFun Identifier LeftParen RightParen LeftBrace "
                   "RightBrace");
  WOLANG_ASSERT_EQ(fixture.stream().comments.size(), size_t{2});
  WOLANG_EXPECT_EQ(fixture.stream().comments[0].begin, 0u);
  WOLANG_EXPECT_EQ(fixture.stream().comments[0].end, 10u);
}

WOLANG_TEST(Lexer, DocCommentsAreTokens) {
  Fixture fixture("/// Fires once.\nfun f() {}");
  WOLANG_EXPECT_EQ(fixture.At(0).kind, TokenKind::kDocComment);
  WOLANG_EXPECT_EQ(fixture.TextAt(0), "/// Fires once.");
  WOLANG_EXPECT_EQ(fixture.At(1).kind, TokenKind::kKwFun);
  WOLANG_EXPECT(fixture.stream().comments.empty());
}

WOLANG_TEST(Lexer, FourSlashesIsAnOrdinaryComment) {
  Fixture fixture("//// divider\nfun f() {}");
  WOLANG_EXPECT_EQ(fixture.At(0).kind, TokenKind::kKwFun);
  WOLANG_EXPECT_EQ(fixture.stream().comments.size(), size_t{1});
}

WOLANG_TEST(Lexer, TracksLineStartsAndBlankLines) {
  Fixture fixture("a b\nc\n\nd");
  WOLANG_EXPECT(fixture.At(0).starts_line());
  WOLANG_EXPECT(!fixture.At(1).starts_line());
  WOLANG_EXPECT(fixture.At(2).starts_line());
  WOLANG_EXPECT(!HasFlag(fixture.At(2).flags, TokenFlags::kBlankLineBefore));
  WOLANG_EXPECT(fixture.At(3).starts_line());
  WOLANG_EXPECT(HasFlag(fixture.At(3).flags, TokenFlags::kBlankLineBefore));
}

WOLANG_TEST(Lexer, HandlesCarriageReturnsAndTabs) {
  Fixture fixture("fun\tf()\r\n{}\r\n");
  WOLANG_EXPECT_EQ(fixture.Kinds(),
                   "KwFun Identifier LeftParen RightParen LeftBrace "
                   "RightBrace");
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, SkipsShebangLine) {
  Fixture fixture("#!/usr/bin/env wolang\nfun main() {}");
  WOLANG_EXPECT_EQ(fixture.At(0).kind, TokenKind::kKwFun);
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, SkipsByteOrderMark) {
  std::string code;
  code.push_back(static_cast<char>(0xEF));
  code.push_back(static_cast<char>(0xBB));
  code.push_back(static_cast<char>(0xBF));
  code.append("fun main() {}");

  Fixture fixture(code);
  WOLANG_EXPECT_EQ(fixture.At(0).kind, TokenKind::kKwFun);
  WOLANG_EXPECT(fixture.Codes().empty());
}

WOLANG_TEST(Lexer, ReportsInvalidUtf8) {
  std::string code = "const x = ";
  code.push_back(static_cast<char>(0xFF));
  code.push_back(static_cast<char>(0xFE));

  Fixture fixture(code);
  WOLANG_EXPECT_EQ(fixture.Codes(), "E0006");
  // One run of bad bytes produces one diagnostic, not one per byte.
  WOLANG_EXPECT_EQ(fixture.At(3).kind, TokenKind::kUnknown);
  WOLANG_EXPECT_EQ(fixture.At(3).length, 2u);
}

// --------------------------------------------------------------- program ----

WOLANG_TEST(Lexer, LexesARepresentativeDeclaration) {
  Fixture fixture(
      "@Spawnable\n"
      "class Turret : Actor {\n"
      "  @Edit @Range(1, 50) var range: float = 15.0\n"
      "  var target: Weak<Pawn>\n"
      "\n"
      "  task fun ScanLoop() uses rand {\n"
      "    loop {\n"
      "      if const t = target.alive {\n"
      "        wait Wait(1.0 / fireRate)\n"
      "      } else {\n"
      "        wait Wait(0.25s)\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n");
  WOLANG_EXPECT(fixture.Codes().empty());
  WOLANG_EXPECT_GT(fixture.count(), size_t{50});
  WOLANG_EXPECT_EQ(fixture.At(0).kind, TokenKind::kAt);
  WOLANG_EXPECT_EQ(fixture.At(2).kind, TokenKind::kKwClass);
}

}  // namespace
}  // namespace wolang
