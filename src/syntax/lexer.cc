// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/syntax/lexer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "wolang/base/check.h"
#include "wolang/base/unicode.h"

namespace wolang {
namespace {

// Diagnostic codes owned by the lexer. The E00xx range is reserved for
// lexical and syntactic problems; see docs/diagnostics.md.
constexpr std::string_view kUnterminatedString = "E0001";
constexpr std::string_view kUnknownEscape = "E0002";
constexpr std::string_view kInvalidCharLiteral = "E0003";
constexpr std::string_view kMalformedNumber = "E0004";
constexpr std::string_view kUnexpectedCharacter = "E0005";
constexpr std::string_view kInvalidUtf8 = "E0006";
constexpr std::string_view kUnterminatedInterpolation = "E0007";
constexpr std::string_view kIntegerOutOfRange = "E0008";
constexpr std::string_view kInvalidUnicodeEscape = "E0009";
constexpr std::string_view kInvalidNumericSuffix = "E0010";
constexpr std::string_view kReservedWord = "E0011";
constexpr std::string_view kUnterminatedComment = "E0012";
constexpr std::string_view kInvalidIdentifier = "E0013";
constexpr std::string_view kNotNormalized = "E0014";

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

bool IsBinaryDigit(char c) {
  return c == '0' || c == '1';
}

bool IsOctalDigit(char c) {
  return c >= '0' && c <= '7';
}

bool IsHexDigit(char c) {
  return IsAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

uint32_t HexValue(char c) {
  if (IsAsciiDigit(c)) {
    return static_cast<uint32_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<uint32_t>(c - 'a') + 10;
  }
  return static_cast<uint32_t>(c - 'A') + 10;
}

bool IsAsciiIdentifierStart(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiIdentifierContinue(char c) {
  return IsAsciiIdentifierStart(c) || IsAsciiDigit(c);
}

bool IsNonAscii(char c) {
  return static_cast<unsigned char>(c) >= 0x80;
}

bool IsOperatorStart(char c) {
  switch (c) {
    case '(':
    case ')':
    case '{':
    case '}':
    case '[':
    case ']':
    case ',':
    case ';':
    case ':':
    case '.':
    case '@':
    case '~':
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '=':
    case '!':
    case '<':
    case '>':
    case '&':
    case '|':
    case '^':
    case '?':
      return true;
    default:
      return false;
  }
}

// True for characters the lexer knows how to begin a token with. Anything else
// is scanned as one run of garbage, so `$$$` is one diagnostic and not three.
bool CanStartToken(char c) {
  return IsAsciiIdentifierStart(c) || IsAsciiDigit(c) || IsNonAscii(c) ||
         IsOperatorStart(c) || c == '"' || c == '\'' || c == ' ' ||
         c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

struct SuffixEntry {
  std::string_view name;
  NumericSuffix suffix;
};

constexpr std::array<SuffixEntry, 10> kNumericSuffixes = {{
    {"s8", NumericSuffix::kI8},
    {"s16", NumericSuffix::kI16},
    {"s32", NumericSuffix::kI32},
    {"s64", NumericSuffix::kI64},
    {"u8", NumericSuffix::kU8},
    {"u16", NumericSuffix::kU16},
    {"u32", NumericSuffix::kU32},
    {"u64", NumericSuffix::kU64},
    {"f32", NumericSuffix::kF32},
    {"f64", NumericSuffix::kF64},
}};

// True for spellings that are clearly meant as a type suffix but name no type,
// such as `f128` or `i7`. Those are reported instead of being read as units.
bool LooksLikeNumericSuffix(std::string_view text) {
  if (text.size() < 2) {
    return false;
  }
  if (text[0] != 'i' && text[0] != 'u' && text[0] != 'f') {
    return false;
  }
  for (size_t i = 1; i < text.size(); ++i) {
    if (!IsAsciiDigit(text[i])) {
      return false;
    }
  }
  return true;
}

class Lexer {
 public:
  // Lexes the half-open byte range [begin, end) of |file|. Truncating the text
  // view at |end| keeps every size-based check below correct, and starting the
  // cursor at |begin| keeps offsets absolute.
  Lexer(const SourceManager& sources,
        FileId file,
        uint32_t begin,
        uint32_t end,
        StringInterner* interner,
        DiagnosticEngine* diagnostics,
        const LexOptions& options)
      : interner_(interner),
        diagnostics_(diagnostics),
        options_(options),
        text_(sources.Text(file).substr(0, end)),
        pos_(begin) {
    stream_.file = file;
    stream_.tokens.reserve((end - begin) / 4 + 8);
  }

  TokenStream Run();

 private:
  bool AtEnd() const { return pos_ >= text_.size(); }

  char Peek(uint32_t ahead = 0) const {
    const size_t index = static_cast<size_t>(pos_) + ahead;
    return index < text_.size() ? text_[index] : '\0';
  }

  SourceSpan Span(uint32_t begin, uint32_t end) const {
    return SourceSpan{stream_.file, begin, end};
  }

  SourceSpan SpanFrom(uint32_t begin) const { return Span(begin, pos_); }

  void Report(std::string_view code,
              std::string message,
              SourceSpan span,
              std::string label = {}) {
    token_has_error_ = true;
    diagnostics_->Error(code, std::move(message))
        .Primary(span, std::move(label));
  }

  void PushToken(TokenKind kind, uint32_t begin, uint32_t aux = 0);
  void PushOperator(TokenKind kind, uint32_t length);

  void SkipByteOrderMark();
  void SkipShebang();
  void SkipTrivia();
  bool AtDocComment() const;
  // Consumes a `/* ... */` comment, which may nest. Returns false when the
  // comment is never closed.
  bool SkipBlockComment();

  void LexToken();
  void LexDocComment();
  void LexIdentifierOrKeyword();
  void LexNumber();
  void LexString(bool is_raw);
  void LexCharLiteral();
  bool LexOperator();
  void LexInvalidByte();

  // Appends the decoded escape sequence at |pos_| to |out|.
  void LexEscape(std::string* out);
  void LexUnicodeEscape(std::string* out);
  void LexInterpolation(StringLiteral* literal);
  void SkipNestedQuoted(char quote);

  StringInterner* interner_;
  DiagnosticEngine* diagnostics_;
  LexOptions options_;
  std::string_view text_;
  uint32_t pos_ = 0;
  TokenFlags pending_flags_ = TokenFlags::kStartsLine;
  bool token_has_error_ = false;
  TokenStream stream_;
};

TokenStream Lexer::Run() {
  // Both only apply to a whole file, never to a sub-range.
  if (pos_ == 0) {
    SkipByteOrderMark();
    SkipShebang();
  }

  for (;;) {
    SkipTrivia();
    if (AtEnd()) {
      break;
    }
    const uint32_t before = pos_;
    LexToken();
    // Every path must consume input; this keeps a bug from hanging the
    // compiler on malformed files.
    WOLANG_DCHECK(pos_ > before);
    if (pos_ <= before) {
      ++pos_;
    }
  }

  Token eof;
  eof.kind = TokenKind::kEndOfFile;
  eof.flags = pending_flags_ | TokenFlags::kStartsLine;
  eof.offset = static_cast<uint32_t>(text_.size());
  eof.length = 0;
  stream_.tokens.push_back(eof);
  return std::move(stream_);
}

void Lexer::PushToken(TokenKind kind, uint32_t begin, uint32_t aux) {
  Token token;
  token.kind = kind;
  token.flags = pending_flags_;
  if (token_has_error_) {
    token.flags |= TokenFlags::kHasError;
  }
  token.offset = begin;
  token.length = pos_ - begin;
  token.aux = aux;
  stream_.tokens.push_back(token);
  pending_flags_ = TokenFlags::kNone;
  token_has_error_ = false;
}

void Lexer::PushOperator(TokenKind kind, uint32_t length) {
  const uint32_t begin = pos_;
  pos_ += length;
  PushToken(kind, begin);
}

void Lexer::SkipByteOrderMark() {
  if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
      static_cast<unsigned char>(text_[1]) == 0xBB &&
      static_cast<unsigned char>(text_[2]) == 0xBF) {
    pos_ = 3;
  }
}

void Lexer::SkipShebang() {
  // A leading "#!" line lets a .wo file be executed directly.
  if (Peek() == '#' && Peek(1) == '!') {
    while (!AtEnd() && text_[pos_] != '\n') {
      ++pos_;
    }
  }
}

bool Lexer::AtDocComment() const {
  return Peek() == '/' && Peek(1) == '/' && Peek(2) == '/' && Peek(3) != '/';
}

void Lexer::SkipTrivia() {
  uint32_t newlines = 0;
  for (;;) {
    if (AtEnd()) {
      break;
    }
    const char c = text_[pos_];
    if (c == '\n') {
      ++pos_;
      ++newlines;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
      ++pos_;
      continue;
    }
    if (c == '/' && Peek(1) == '/') {
      if (AtDocComment()) {
        break;
      }
      const uint32_t begin = pos_;
      while (!AtEnd() && text_[pos_] != '\n') {
        ++pos_;
      }
      if (options_.keep_comments) {
        stream_.comments.push_back(SpanFrom(begin));
      }
      continue;
    }
    if (c == '/' && Peek(1) == '*') {
      const uint32_t begin = pos_;
      const bool closed = SkipBlockComment();
      if (options_.keep_comments) {
        stream_.comments.push_back(SpanFrom(begin));
      }
      if (!closed) {
        break;
      }
      continue;
    }
    break;
  }

  if (newlines > 0) {
    pending_flags_ |= TokenFlags::kStartsLine;
  }
  if (newlines > 1) {
    pending_flags_ |= TokenFlags::kBlankLineBefore;
  }
}

bool Lexer::SkipBlockComment() {
  const uint32_t begin = pos_;
  pos_ += 2;
  // Nesting is allowed so that commenting out a region containing a comment
  // works, which is the only reason to reach for a block comment at all.
  int depth = 1;
  while (!AtEnd()) {
    if (Peek() == '/' && Peek(1) == '*') {
      ++depth;
      pos_ += 2;
      continue;
    }
    if (Peek() == '*' && Peek(1) == '/') {
      --depth;
      pos_ += 2;
      if (depth == 0) {
        return true;
      }
      continue;
    }
    ++pos_;
  }

  Report(kUnterminatedComment, "unterminated block comment",
         Span(begin, begin + 2), "`/*` opened here");
  return false;
}

void Lexer::LexToken() {
  const char c = Peek();

  if (c == '/' && AtDocComment()) {
    LexDocComment();
    return;
  }
  // Raw strings must be recognized before identifiers, because `r` on its own
  // is a perfectly good identifier start.
  if (c == 'r' && Peek(1) == '"') {
    LexString(/*is_raw=*/true);
    return;
  }
  if (IsAsciiIdentifierStart(c)) {
    LexIdentifierOrKeyword();
    return;
  }
  if (IsNonAscii(c)) {
    uint32_t code_point = 0;
    const uint32_t length = DecodeUtf8(text_, pos_, &code_point);
    if (length == 0) {
      LexInvalidByte();
    } else if (IsIdentifierStart(code_point)) {
      LexIdentifierOrKeyword();
    } else {
      // Well-formed UTF-8 that is not a letter: an emoji, a symbol, or an
      // invisible formatting character. Naming something with it would make
      // two different names look identical.
      const uint32_t begin = pos_;
      pos_ += length;
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "U+%04X", code_point);
      std::string message = "`";
      message.append(buffer);
      message.append("` cannot appear in an identifier");
      Report(kInvalidIdentifier, std::move(message), SpanFrom(begin),
             "identifiers are letters, digits and `_`");
      PushToken(TokenKind::kUnknown, begin);
    }
    return;
  }
  if (IsAsciiDigit(c)) {
    LexNumber();
    return;
  }
  if (c == '"') {
    LexString(/*is_raw=*/false);
    return;
  }
  if (c == '\'') {
    LexCharLiteral();
    return;
  }
  if (LexOperator()) {
    return;
  }

  const uint32_t begin = pos_;
  while (!AtEnd() && !CanStartToken(text_[pos_])) {
    ++pos_;
  }
  const std::string_view run = text_.substr(begin, pos_ - begin);
  std::string message =
      run.size() == 1 ? "unexpected character `" : "unexpected characters `";
  message.append(run);
  message.append("`");
  Report(kUnexpectedCharacter, std::move(message), SpanFrom(begin));
  PushToken(TokenKind::kUnknown, begin);
}

void Lexer::LexInvalidByte() {
  const uint32_t begin = pos_;
  // Consume the whole run of malformed bytes so one broken sequence yields one
  // diagnostic instead of one per byte.
  while (!AtEnd() && IsNonAscii(text_[pos_]) &&
         DecodeUtf8(text_, pos_, nullptr) == 0) {
    ++pos_;
  }
  Report(kInvalidUtf8, "invalid UTF-8 encoding", SpanFrom(begin),
         "source files must be valid UTF-8");
  PushToken(TokenKind::kUnknown, begin);
}

void Lexer::LexDocComment() {
  const uint32_t begin = pos_;
  while (!AtEnd() && text_[pos_] != '\n') {
    ++pos_;
  }
  PushToken(TokenKind::kDocComment, begin);
}

void Lexer::LexIdentifierOrKeyword() {
  const uint32_t begin = pos_;
  // The first code point was already validated as a start character by the
  // caller; the loop only has to handle continuation.
  while (!AtEnd()) {
    const char c = text_[pos_];
    if (IsAsciiIdentifierContinue(c)) {
      ++pos_;
      continue;
    }
    if (IsNonAscii(c)) {
      uint32_t code_point = 0;
      const uint32_t length = DecodeUtf8(text_, pos_, &code_point);
      if (length == 0 || !IsIdentifierContinue(code_point)) {
        break;
      }
      pos_ += length;
      continue;
    }
    break;
  }

  const std::string_view spelling = text_.substr(begin, pos_ - begin);

  // Names must already be in NFC rather than being normalized here: that is
  // what guarantees one name has one spelling, and a rejected name is an
  // actionable message instead of a silent rewrite.
  if (!IsNormalizedNfc(spelling)) {
    Report(kNotNormalized, "identifier is not in Unicode NFC form",
           SpanFrom(begin),
           "write the composed form, as most editors already do");
  }
  if (spelling == "_") {
    PushToken(TokenKind::kUnderscore, begin);
    return;
  }

  const TokenKind keyword = LookupKeyword(spelling);
  const Symbol symbol = interner_->Intern(spelling);
  if (keyword != TokenKind::kIdentifier) {
    PushToken(keyword, begin, symbol.index);
    return;
  }

  if (IsReservedWord(spelling)) {
    // `trait` is the one reserved word that used to *be* a keyword, so it
    // says what to write now rather than "pick a different name" (D104).
    if (spelling == "trait") {
      Report(kReservedWord, "`trait` is now spelled `ability`",
             SpanFrom(begin), "write `ability` here");
    } else {
      std::string message = "`";
      message.append(spelling);
      message.append("` is reserved for future use");
      Report(kReservedWord, std::move(message), SpanFrom(begin),
             "pick a different name");
    }
  }
  PushToken(TokenKind::kIdentifier, begin, symbol.index);
}

void Lexer::LexNumber() {
  const uint32_t begin = pos_;
  NumberLiteral literal;

  bool overflow = false;
  uint64_t integer_value = 0;
  const auto accumulate = [&](uint32_t digit, uint64_t radix) {
    if (integer_value > (UINT64_MAX - digit) / radix) {
      overflow = true;
      return;
    }
    integer_value = integer_value * radix + digit;
  };

  uint32_t digit_count = 0;
  if (Peek() == '0' && (Peek(1) == 'x' || Peek(1) == 'X' || Peek(1) == 'b' ||
                        Peek(1) == 'B' || Peek(1) == 'o' || Peek(1) == 'O')) {
    const char marker = Peek(1);
    uint64_t radix = 16;
    if (marker == 'x' || marker == 'X') {
      literal.base = NumericBase::kHexadecimal;
      radix = 16;
    } else if (marker == 'b' || marker == 'B') {
      literal.base = NumericBase::kBinary;
      radix = 2;
    } else {
      literal.base = NumericBase::kOctal;
      radix = 8;
    }
    pos_ += 2;

    for (;;) {
      const char c = Peek();
      if (c == '_') {
        ++pos_;
        continue;
      }
      const bool valid = radix == 16  ? IsHexDigit(c)
                         : radix == 8 ? IsOctalDigit(c)
                                      : IsBinaryDigit(c);
      if (!valid) {
        break;
      }
      accumulate(HexValue(c), radix);
      ++digit_count;
      ++pos_;
    }

    if (digit_count == 0) {
      Report(kMalformedNumber, "expected digits after the base prefix",
             SpanFrom(begin));
    } else if (IsAsciiDigit(Peek()) || Peek() == '.') {
      // `0b1012` or `0x1.8`: the trailing part is not valid for this base.
      const uint32_t bad = pos_;
      while (!AtEnd() &&
             (IsAsciiDigit(text_[pos_]) || text_[pos_] == '.' ||
              text_[pos_] == '_')) {
        ++pos_;
      }
      Report(kMalformedNumber, "invalid digits for this base", SpanFrom(bad));
    }
  } else {
    for (;;) {
      const char c = Peek();
      if (c == '_') {
        ++pos_;
        continue;
      }
      if (!IsAsciiDigit(c)) {
        break;
      }
      accumulate(static_cast<uint32_t>(c - '0'), 10);
      ++digit_count;
      ++pos_;
    }

    // A dot only starts a fraction when a digit follows, so `0..10` stays a
    // range and `1.max()` stays a method call.
    if (Peek() == '.' && IsAsciiDigit(Peek(1))) {
      literal.is_float = true;
      ++pos_;
      while (IsAsciiDigit(Peek()) || Peek() == '_') {
        ++pos_;
      }
    }

    if (Peek() == 'e' || Peek() == 'E') {
      const bool signed_exponent = Peek(1) == '+' || Peek(1) == '-';
      const char after_sign = signed_exponent ? Peek(2) : Peek(1);
      if (IsAsciiDigit(after_sign)) {
        literal.is_float = true;
        pos_ += signed_exponent ? 2 : 1;
        while (IsAsciiDigit(Peek()) || Peek() == '_') {
          ++pos_;
        }
      }
    }
  }

  const uint32_t digits_end = pos_;

  // Suffix: either a numeric type (`u8`, `f64`) or a unit (`ms`, `deg`).
  if (IsAsciiIdentifierStart(Peek())) {
    const uint32_t suffix_begin = pos_;
    while (IsAsciiIdentifierContinue(Peek())) {
      ++pos_;
    }
    const std::string_view suffix =
        text_.substr(suffix_begin, pos_ - suffix_begin);

    bool matched = false;
    for (const SuffixEntry& entry : kNumericSuffixes) {
      if (entry.name != suffix) {
        continue;
      }
      matched = true;
      const bool suffix_is_float = entry.suffix == NumericSuffix::kF32 ||
                                   entry.suffix == NumericSuffix::kF64;
      if (literal.is_float && !suffix_is_float) {
        std::string message = "cannot apply integer suffix `";
        message.append(suffix);
        message.append("` to a floating point literal");
        Report(kInvalidNumericSuffix, std::move(message),
               SpanFrom(suffix_begin));
      } else {
        literal.suffix = entry.suffix;
        literal.is_float = literal.is_float || suffix_is_float;
      }
      break;
    }

    if (!matched) {
      if (LooksLikeNumericSuffix(suffix)) {
        std::string message = "unknown numeric suffix `";
        message.append(suffix);
        message.append("`");
        Report(kInvalidNumericSuffix, std::move(message),
               SpanFrom(suffix_begin),
               "known suffixes are i8..i64, u8..u64, f32 and f64");
      } else if (literal.base != NumericBase::kDecimal) {
        Report(kInvalidNumericSuffix,
               "a unit suffix requires a decimal literal",
               SpanFrom(suffix_begin));
      } else {
        literal.unit = interner_->Intern(suffix);
      }
    }
  }

  if (overflow) {
    Report(kIntegerOutOfRange,
           "integer literal does not fit in 64 bits",
           SpanFrom(begin));
  }

  literal.integer_value = integer_value;
  if (literal.is_float) {
    // strtod wants a clean buffer: digit separators and the suffix are not
    // part of the value.
    std::string buffer;
    buffer.reserve(digits_end - begin);
    for (uint32_t i = begin; i < digits_end; ++i) {
      if (text_[i] != '_') {
        buffer.push_back(text_[i]);
      }
    }
    literal.float_value = std::strtod(buffer.c_str(), nullptr);
  } else {
    literal.float_value = static_cast<double>(integer_value);
  }

  const auto index = static_cast<uint32_t>(stream_.numbers.size());
  stream_.numbers.push_back(literal);
  PushToken(literal.is_float ? TokenKind::kFloatLiteral
                             : TokenKind::kIntLiteral,
            begin, index);
}

void Lexer::LexString(bool is_raw) {
  const uint32_t begin = pos_;
  pos_ += is_raw ? 2 : 1;

  StringLiteral literal;
  literal.is_raw = is_raw;

  std::string text;
  uint32_t text_begin = pos_;
  const auto flush = [&](uint32_t end) {
    if (text.empty() && !literal.parts.empty()) {
      return;
    }
    if (text.empty() && literal.parts.empty() && end == text_begin) {
      return;
    }
    StringPart part;
    part.kind = StringPartKind::kText;
    part.text = std::move(text);
    part.span = Span(text_begin, end);
    literal.parts.push_back(std::move(part));
    text.clear();
  };

  bool terminated = false;
  while (!AtEnd()) {
    const char c = text_[pos_];
    if (c == '"') {
      flush(pos_);
      ++pos_;
      terminated = true;
      break;
    }
    if (c == '\n' && !is_raw) {
      break;
    }
    if (!is_raw && c == '\\') {
      LexEscape(&text);
      continue;
    }
    if (!is_raw && c == '{') {
      flush(pos_);
      LexInterpolation(&literal);
      text_begin = pos_;
      continue;
    }
    if (IsNonAscii(c)) {
      const uint32_t length = DecodeUtf8(text_, pos_, nullptr);
      if (length == 0) {
        Report(kInvalidUtf8, "invalid UTF-8 encoding in string literal",
               Span(pos_, pos_ + 1));
        ++pos_;
        continue;
      }
      text.append(text_.substr(pos_, length));
      pos_ += length;
      continue;
    }
    text.push_back(c);
    ++pos_;
  }

  if (!terminated) {
    flush(pos_);
    Report(kUnterminatedString, "unterminated string literal",
           Span(begin, begin + (is_raw ? 2 : 1)), "literal starts here");
  }

  const auto index = static_cast<uint32_t>(stream_.strings.size());
  stream_.strings.push_back(std::move(literal));
  PushToken(TokenKind::kStringLiteral, begin, index);
}

void Lexer::LexEscape(std::string* out) {
  const uint32_t begin = pos_;
  ++pos_;  // Consume the backslash.
  if (AtEnd()) {
    Report(kUnterminatedString, "unterminated escape sequence",
           SpanFrom(begin));
    return;
  }

  const char escape = text_[pos_++];
  switch (escape) {
    case 'n':
      out->push_back('\n');
      return;
    case 'r':
      out->push_back('\r');
      return;
    case 't':
      out->push_back('\t');
      return;
    case '0':
      out->push_back('\0');
      return;
    case '\\':
    case '"':
    case '\'':
    case '{':
    case '}':
      out->push_back(escape);
      return;
    case 'u':
      LexUnicodeEscape(out);
      return;
    default:
      break;
  }

  std::string message = "unknown escape sequence `\\";
  message.push_back(escape);
  message.append("`");
  Report(kUnknownEscape, std::move(message), SpanFrom(begin),
         "valid escapes are \\n \\r \\t \\0 \\\\ \\\" \\' \\{ \\} \\u{...}");
  out->push_back(escape);
}

void Lexer::LexUnicodeEscape(std::string* out) {
  const uint32_t begin = pos_ - 2;  // Includes the "\u".
  if (Peek() != '{') {
    Report(kInvalidUnicodeEscape, "expected `{` after `\\u`", SpanFrom(begin),
           "write a code point as \\u{1F600}");
    return;
  }
  ++pos_;

  uint32_t value = 0;
  uint32_t digits = 0;
  while (IsHexDigit(Peek()) && digits < 6) {
    value = value * 16 + HexValue(Peek());
    ++digits;
    ++pos_;
  }

  if (digits == 0) {
    Report(kInvalidUnicodeEscape, "expected hexadecimal digits in `\\u{...}`",
           SpanFrom(begin));
    return;
  }
  if (Peek() != '}') {
    Report(kInvalidUnicodeEscape, "expected `}` to close `\\u{`",
           SpanFrom(begin));
    return;
  }
  ++pos_;

  if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    Report(kInvalidUnicodeEscape, "code point is not a valid scalar value",
           SpanFrom(begin),
           "valid code points are 0..0x10FFFF excluding surrogates");
    return;
  }
  AppendUtf8(value, out);
}

void Lexer::SkipNestedQuoted(char quote) {
  ++pos_;  // Consume the opening quote.
  while (!AtEnd() && text_[pos_] != quote && text_[pos_] != '\n') {
    if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
      pos_ += 2;
      continue;
    }
    ++pos_;
  }
  if (!AtEnd() && text_[pos_] == quote) {
    ++pos_;
  }
}

void Lexer::LexInterpolation(StringLiteral* literal) {
  const uint32_t brace = pos_;
  ++pos_;  // Consume '{'.
  const uint32_t expression_begin = pos_;

  int depth = 1;
  while (!AtEnd()) {
    const char c = text_[pos_];
    if (c == '\n') {
      break;
    }
    if (c == '"' || c == '\'') {
      SkipNestedQuoted(c);
      continue;
    }
    if (c == '{') {
      ++depth;
      ++pos_;
      continue;
    }
    if (c == '}') {
      --depth;
      ++pos_;
      if (depth == 0) {
        break;
      }
      continue;
    }
    ++pos_;
  }

  const bool closed = depth == 0;
  const uint32_t expression_end = closed ? pos_ - 1 : pos_;
  if (!closed) {
    Report(kUnterminatedInterpolation, "unterminated interpolation",
           Span(brace, brace + 1), "`{` opened here");
  } else if (expression_end == expression_begin) {
    Report(kUnterminatedInterpolation, "interpolation is empty",
           Span(brace, pos_), "put an expression between the braces");
  }

  StringPart part;
  part.kind = StringPartKind::kInterpolation;
  part.span = Span(expression_begin, expression_end);
  literal->parts.push_back(std::move(part));
}

void Lexer::LexCharLiteral() {
  const uint32_t begin = pos_;
  ++pos_;  // Consume the opening quote.

  if (AtEnd() || text_[pos_] == '\n') {
    Report(kInvalidCharLiteral, "unterminated character literal",
           SpanFrom(begin));
    PushToken(TokenKind::kCharLiteral, begin, 0);
    return;
  }

  if (text_[pos_] == '\'') {
    ++pos_;
    Report(kInvalidCharLiteral, "empty character literal", SpanFrom(begin),
           "a character literal holds exactly one character");
    PushToken(TokenKind::kCharLiteral, begin, 0);
    return;
  }

  std::string decoded;
  if (text_[pos_] == '\\') {
    LexEscape(&decoded);
  } else if (IsNonAscii(text_[pos_])) {
    const uint32_t length = DecodeUtf8(text_, pos_, nullptr);
    if (length == 0) {
      Report(kInvalidUtf8, "invalid UTF-8 encoding in character literal",
             Span(pos_, pos_ + 1));
      ++pos_;
    } else {
      decoded.append(text_.substr(pos_, length));
      pos_ += length;
    }
  } else {
    decoded.push_back(text_[pos_]);
    ++pos_;
  }

  uint32_t code_point = 0;
  const uint32_t decoded_length =
      decoded.empty() ? 0 : DecodeUtf8(decoded, 0, &code_point);
  if (decoded_length != 0 && decoded_length != decoded.size()) {
    Report(kInvalidCharLiteral,
           "character literal must hold exactly one character",
           SpanFrom(begin));
  }

  if (AtEnd() || text_[pos_] != '\'') {
    Report(kInvalidCharLiteral, "unterminated character literal",
           SpanFrom(begin), "expected a closing `'`");
  } else {
    ++pos_;
  }

  PushToken(TokenKind::kCharLiteral, begin, code_point);
}

bool Lexer::LexOperator() {
  switch (Peek()) {
    case '(':
      PushOperator(TokenKind::kLeftParen, 1);
      return true;
    case ')':
      PushOperator(TokenKind::kRightParen, 1);
      return true;
    case '{':
      PushOperator(TokenKind::kLeftBrace, 1);
      return true;
    case '}':
      PushOperator(TokenKind::kRightBrace, 1);
      return true;
    case '[':
      PushOperator(TokenKind::kLeftBracket, 1);
      return true;
    case ']':
      PushOperator(TokenKind::kRightBracket, 1);
      return true;
    case ',':
      PushOperator(TokenKind::kComma, 1);
      return true;
    case ';':
      PushOperator(TokenKind::kSemicolon, 1);
      return true;
    case ':':
      PushOperator(TokenKind::kColon, 1);
      return true;
    case '@':
      PushOperator(TokenKind::kAt, 1);
      return true;
    case '~':
      PushOperator(TokenKind::kTilde, 1);
      return true;
    case '.':
      if (Peek(1) == '.' && Peek(2) == '=') {
        PushOperator(TokenKind::kDotDotEq, 3);
      } else if (Peek(1) == '.') {
        PushOperator(TokenKind::kDotDot, 2);
      } else {
        PushOperator(TokenKind::kDot, 1);
      }
      return true;
    case '+':
      PushOperator(Peek(1) == '=' ? TokenKind::kPlusEq : TokenKind::kPlus,
                   Peek(1) == '=' ? 2 : 1);
      return true;
    case '-':
      if (Peek(1) == '>') {
        PushOperator(TokenKind::kArrow, 2);
      } else if (Peek(1) == '=') {
        PushOperator(TokenKind::kMinusEq, 2);
      } else {
        PushOperator(TokenKind::kMinus, 1);
      }
      return true;
    case '*':
      PushOperator(Peek(1) == '=' ? TokenKind::kStarEq : TokenKind::kStar,
                   Peek(1) == '=' ? 2 : 1);
      return true;
    case '/':
      PushOperator(Peek(1) == '=' ? TokenKind::kSlashEq : TokenKind::kSlash,
                   Peek(1) == '=' ? 2 : 1);
      return true;
    case '%':
      PushOperator(Peek(1) == '=' ? TokenKind::kPercentEq
                                  : TokenKind::kPercent,
                   Peek(1) == '=' ? 2 : 1);
      return true;
    case '^':
      PushOperator(Peek(1) == '=' ? TokenKind::kCaretEq : TokenKind::kCaret,
                   Peek(1) == '=' ? 2 : 1);
      return true;
    case '!':
      PushOperator(Peek(1) == '=' ? TokenKind::kBangEq : TokenKind::kBang,
                   Peek(1) == '=' ? 2 : 1);
      return true;
    case '=':
      if (Peek(1) == '=') {
        PushOperator(TokenKind::kEqEq, 2);
      } else if (Peek(1) == '>') {
        PushOperator(TokenKind::kFatArrow, 2);
      } else {
        PushOperator(TokenKind::kEq, 1);
      }
      return true;
    case '<':
      if (Peek(1) == '<' && Peek(2) == '=') {
        PushOperator(TokenKind::kShlEq, 3);
      } else if (Peek(1) == '<') {
        PushOperator(TokenKind::kShl, 2);
      } else if (Peek(1) == '=') {
        PushOperator(TokenKind::kLessEq, 2);
      } else {
        PushOperator(TokenKind::kLess, 1);
      }
      return true;
    case '>':
      if (Peek(1) == '>' && Peek(2) == '=') {
        PushOperator(TokenKind::kShrEq, 3);
      } else if (Peek(1) == '>') {
        PushOperator(TokenKind::kShr, 2);
      } else if (Peek(1) == '=') {
        PushOperator(TokenKind::kGreaterEq, 2);
      } else {
        PushOperator(TokenKind::kGreater, 1);
      }
      return true;
    case '&':
      if (Peek(1) == '&') {
        PushOperator(TokenKind::kAmpAmp, 2);
      } else if (Peek(1) == '=') {
        PushOperator(TokenKind::kAmpEq, 2);
      } else {
        PushOperator(TokenKind::kAmp, 1);
      }
      return true;
    case '|':
      if (Peek(1) == '|') {
        PushOperator(TokenKind::kPipePipe, 2);
      } else if (Peek(1) == '=') {
        PushOperator(TokenKind::kPipeEq, 2);
      } else {
        PushOperator(TokenKind::kPipe, 1);
      }
      return true;
    case '?':
      if (Peek(1) == '?' && Peek(2) == '=') {
        PushOperator(TokenKind::kQuestionQuestionEq, 3);
      } else if (Peek(1) == '?') {
        PushOperator(TokenKind::kQuestionQuestion, 2);
      } else if (Peek(1) == '.') {
        PushOperator(TokenKind::kQuestionDot, 2);
      } else {
        PushOperator(TokenKind::kQuestion, 1);
      }
      return true;
    default:
      return false;
  }
}

}  // namespace

TokenStream Lex(const SourceManager& sources,
                FileId file,
                StringInterner* interner,
                DiagnosticEngine* diagnostics,
                const LexOptions& options) {
  WOLANG_CHECK(interner != nullptr);
  WOLANG_CHECK(diagnostics != nullptr);
  const auto size = static_cast<uint32_t>(sources.Text(file).size());
  Lexer lexer(sources, file, 0, size, interner, diagnostics, options);
  return lexer.Run();
}

TokenStream LexSpan(const SourceManager& sources,
                    SourceSpan span,
                    StringInterner* interner,
                    DiagnosticEngine* diagnostics,
                    const LexOptions& options) {
  WOLANG_CHECK(interner != nullptr);
  WOLANG_CHECK(diagnostics != nullptr);
  const auto size = static_cast<uint32_t>(sources.Text(span.file).size());
  const uint32_t end = std::min(span.end, size);
  const uint32_t begin = std::min(span.begin, end);
  Lexer lexer(sources, span.file, begin, end, interner, diagnostics, options);
  return lexer.Run();
}

}  // namespace wolang
