// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/parser.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wolang/base/check.h"
#include "wolang/syntax/lexer.h"

namespace wolang {
namespace {

// Diagnostic codes owned by the parser; see docs/diagnostics.md.
constexpr std::string_view kExpectedDeclaration = "E0020";
constexpr std::string_view kExpectedIdentifier = "E0021";
constexpr std::string_view kExpectedToken = "E0022";
constexpr std::string_view kExpectedType = "E0023";
constexpr std::string_view kExpectedExpression = "E0024";
constexpr std::string_view kExpectedPattern = "E0025";
constexpr std::string_view kUnmatchedDelimiter = "E0026";
constexpr std::string_view kExpectedBlock = "E0028";
constexpr std::string_view kDuplicateModifier = "E0029";
constexpr std::string_view kExpectedMatchArm = "E0034";
constexpr std::string_view kExpectedState = "E0035";
constexpr std::string_view kExpectedTransition = "E0036";
constexpr std::string_view kExpectedUnitDefinition = "E0037";
constexpr std::string_view kExpectedStatementEnd = "E0040";
constexpr std::string_view kNestingTooDeep = "E0041";
constexpr std::string_view kUnknownTypeParameter = "E0042";
constexpr std::string_view kTrailingTokens = "E0043";
constexpr std::string_view kUnknownLabel = "E0044";
constexpr std::string_view kExpectedArraySize = "E0045";

// True for tokens that can begin an expression. Used to tell `a..b` from
// `a..`, where the range simply ends. |allow_brace| is false in a loop or
// condition subject, where `{` opens the body instead.
bool StartsExpression(TokenKind kind, bool allow_brace) {
  if (kind == TokenKind::kLeftBrace) {
    return allow_brace;
  }
  switch (kind) {
    case TokenKind::kIdentifier:
    case TokenKind::kIntLiteral:
    case TokenKind::kFloatLiteral:
    case TokenKind::kStringLiteral:
    case TokenKind::kCharLiteral:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse:
    case TokenKind::kKwNull:
    case TokenKind::kKwSelf:
    case TokenKind::kLeftParen:
    case TokenKind::kLeftBracket:
    case TokenKind::kMinus:
    case TokenKind::kBang:
    case TokenKind::kTilde:
    case TokenKind::kStar:
    case TokenKind::kAmp:
    case TokenKind::kPipe:
    case TokenKind::kPipePipe:
    case TokenKind::kKwWait:
    case TokenKind::kKwSpawn:
    case TokenKind::kKwIf:
    case TokenKind::kKwMatch:
    case TokenKind::kKwLoop:
    case TokenKind::kKwWhile:
    case TokenKind::kKwFor:
    case TokenKind::kKwForeach:
    case TokenKind::kKwUnsafe:
    case TokenKind::kKwArena:
      return true;
    default:
      return false;
  }
}

bool StartsLoop(TokenKind kind) {
  switch (kind) {
    case TokenKind::kKwLoop:
    case TokenKind::kKwWhile:
    case TokenKind::kKwFor:
    case TokenKind::kKwForeach:
      return true;
    default:
      return false;
  }
}

bool StartsItem(TokenKind kind) {
  switch (kind) {
    case TokenKind::kAt:
    case TokenKind::kKwOffer:
    case TokenKind::kKwPriv:
    case TokenKind::kKwComptime:
    case TokenKind::kKwExtern:
    case TokenKind::kKwTask:
    case TokenKind::kKwVirtual:
    case TokenKind::kKwOverride:
    case TokenKind::kKwFun:
    case TokenKind::kKwClass:
    case TokenKind::kKwStruct:
    case TokenKind::kKwEnum:
    case TokenKind::kKwTrait:
    case TokenKind::kKwExtend:
    case TokenKind::kKwComponent:
    case TokenKind::kKwTag:
    case TokenKind::kKwSystem:
    case TokenKind::kKwSignal:
    case TokenKind::kKwOn:
    case TokenKind::kKwNeed:
    case TokenKind::kKwType:
    case TokenKind::kKwError:
      return true;
    default:
      return false;
  }
}

// Tokens that may appear inside a generic argument list when the parser is
// guessing whether `<` opens one. Parentheses are deliberately excluded: that
// keeps `a < f(x) > (y)` a pair of comparisons, at the cost of not accepting a
// function type as an explicit generic argument in expression position.
bool CanAppearInGenericArguments(TokenKind kind) {
  switch (kind) {
    case TokenKind::kIdentifier:
    case TokenKind::kDot:
    case TokenKind::kComma:
    case TokenKind::kQuestion:
    case TokenKind::kStar:
    case TokenKind::kAmp:
    case TokenKind::kKwMut:
    case TokenKind::kUnderscore:
    case TokenKind::kIntLiteral:
      return true;
    default:
      return false;
  }
}

constexpr std::array kContextualKeywords = {
#define WOLANG_CONTEXTUAL(Name, Spelling) ContextualKeyword::k##Name,
#include "wolang/syntax/token_kind.inc"
};

class Parser {
 public:
  Parser(const SourceManager& sources,
         const TokenStream& tokens,
         StringInterner* interner,
         DiagnosticEngine* diagnostics,
         Ast* ast,
         const ParseOptions& options)
      : sources_(sources),
        tokens_(tokens),
        interner_(interner),
        diagnostics_(diagnostics),
        ast_(ast),
        options_(options) {
    for (ContextualKeyword keyword : kContextualKeywords) {
      contextual_[static_cast<size_t>(keyword)] =
          interner_->Intern(ContextualKeywordSpelling(keyword));
    }
  }

  void ParseFile();

  // Parses a single expression and reports anything left over. Used for the
  // expressions inside string interpolations.
  ExprId ParseWholeExpression();

 private:
  // --------------------------------------------------------------- cursor --

  const Token& Peek(size_t ahead = 0) const {
    return tokens_.At(pos_ + ahead);
  }
  TokenKind PeekKind(size_t ahead = 0) const { return Peek(ahead).kind; }
  bool At(TokenKind kind) const { return PeekKind() == kind; }
  bool AtEnd() const { return At(TokenKind::kEndOfFile); }

  bool AtContextual(ContextualKeyword keyword, size_t ahead = 0) const {
    const Token& token = Peek(ahead);
    return token.kind == TokenKind::kIdentifier &&
           tokens_.IdentifierOf(token) ==
               contextual_[static_cast<size_t>(keyword)];
  }

  const Token& Advance() {
    const Token& token = Peek();
    if (!AtEnd()) {
      ++pos_;
    }
    return token;
  }

  bool Eat(TokenKind kind) {
    if (!At(kind)) {
      return false;
    }
    Advance();
    return true;
  }

  SourceSpan SpanOf(const Token& token) const {
    return tokens_.SpanOf(token);
  }
  SourceSpan CurrentSpan() const { return SpanOf(Peek()); }

  // The span from the token at |start| through the token before the cursor.
  SourceSpan SpanFrom(size_t start) const {
    const SourceSpan first = SpanOf(tokens_.At(start));
    const size_t last = pos_ == 0 ? 0 : pos_ - 1;
    return first.Merge(SpanOf(tokens_.At(last < start ? start : last)));
  }

  std::string_view TextOf(const Token& token) const {
    return sources_.SpanText(SpanOf(token));
  }

  // ----------------------------------------------------------- reporting --

  bool Report(std::string_view code,
              std::string message,
              SourceSpan span,
              std::string label = {});

  std::string DescribeCurrent() const;
  bool ExpectToken(TokenKind kind);
  // Stores the span of the identifier in |span| when it is not null.
  Symbol ExpectIdentifier(SourceSpan* span = nullptr);
  // Consumes a closing brace, blaming |open| when it is missing.
  bool ExpectCloseBrace(SourceSpan open);
  void ExpectStatementEnd();

  void SkipToItemBoundary();
  void SkipToStatementBoundary();
  void SkipToMachineBoundary();
  void SkipBalancedTo(TokenKind closing);

  // --------------------------------------------------------------- items --

  ItemId ParseItem(bool allow_error_item);
  SourceSpan CollectDocComments();
  void ParseAttributes(std::vector<AttributeId>* out);
  ItemFlags ParseModifiers(Symbol* abi);
  AstRange<ItemId> ParseItemBlock(SourceSpan open);

  ItemId ParseImport(Item item, size_t start);
  ItemId ParseFunction(Item item, size_t start);
  ItemId ParseAggregate(Item item, size_t start, ItemKind kind);
  ItemId ParseEnum(Item item, size_t start);
  ItemId ParseExtend(Item item, size_t start);
  ItemId ParseErrorSet(Item item, size_t start);
  ItemId ParseTag(Item item, size_t start);
  ItemId ParseSystem(Item item, size_t start);
  ItemId ParseSignal(Item item, size_t start);
  ItemId ParseOn(Item item, size_t start);
  ItemId ParseBinding(Item item, size_t start);
  ItemId ParseTypeAlias(Item item, size_t start);
  ItemId ParseDimension(Item item, size_t start);
  ItemId ParseUnit(Item item, size_t start);
  ItemId ParseEffect(Item item, size_t start);
  ItemId ParseMachine(Item item, size_t start);
  StateId ParseState();
  bool ParseTransition(std::vector<TransitionId>* out);

  AstRange<ParamId> ParseParameterList(bool types_required);
  AstRange<GenericParamId> ParseGenericParameters();
  void ParseWhereClause(std::vector<GenericParamId>* generics);
  AstRange<TypeRefId> ParseEffectList();

  // ---------------------------------------------------------- statements --

  ExprId ParseBlock();
  StmtId ParseStatement();

  // --------------------------------------------------------- expressions --

  ExprId ParseExpression(Precedence min_precedence = Precedence::kAssign);
  ExprId ParseUnary();
  ExprId ParsePostfix(ExprId operand, size_t start);
  ExprId ParsePrimary();
  ExprId ParseIf();
  ExprId ParseMatch();
  ExprId ParseLoopLike(TokenKind keyword, Symbol label = Symbol{});
  ExprId ParseClosure();
  ExprId ParseStringLiteral();
  ExprId ParseInterpolation(SourceSpan span);
  ExprId ParseParenthesized();
  ExprId ParseArrayLiteral();
  ExprId ParseConditionWithOptionalBinding(PatternId* pattern);
  AstRange<ExprId> ParseCallArguments();
  bool LooksLikeGenericCall() const;
  ExprId MakeError(size_t start);

  // ------------------------------------------------------------ patterns --

  bool AtPatternStart() const;
  // Consumes one `?` of an optional pattern, splitting a `??` token.
  bool EatPatternQuestion();
  PatternId ParsePattern();
  AstRange<PatternId> ParsePatternAlternatives();

  // --------------------------------------------------------------- types --

  TypeRefId ParseType();
  TypeRefId ParseTypeNoSuffix();
  AstRange<TypeRefId> ParseGenericArguments();
  AstRange<Symbol> ParseDottedPath();
  // Consumes the `>` that closes a generic list, splitting a `>>` token.
  bool EatAngleClose();
  bool AtAngleClose() const;

  // --------------------------------------------------------------- state --

  const SourceManager& sources_;
  const TokenStream& tokens_;
  StringInterner* interner_;
  DiagnosticEngine* diagnostics_;
  Ast* ast_;
  ParseOptions options_;

  size_t pos_ = 0;
  uint32_t errors_ = 0;
  uint32_t depth_ = 0;
  // Set after consuming the first half of a `>>` token that closed a nested
  // generic argument list.
  bool pending_angle_close_ = false;
  // Set after consuming the first half of a `??` token in a pattern.
  bool pending_question_ = false;
  // Cleared while parsing the subject of `if`, `while`, `for`, `foreach` and
  // `match`, where `{` starts the body rather than a struct literal.
  bool allow_struct_literal_ = true;

  std::array<Symbol, static_cast<size_t>(ContextualKeyword::kCount)>
      contextual_{};
  // Labels of the loops currently being parsed, innermost last.
  std::vector<Symbol> labels_;
};

constexpr uint32_t kMaxNesting = 192;

Precedence Tighter(Precedence precedence) {
  return static_cast<Precedence>(static_cast<int>(precedence) + 1);
}

// Restores a boolean on scope exit, for the struct-literal context flag.
class ScopedFlag {
 public:
  ScopedFlag(bool* slot, bool value) : slot_(slot), saved_(*slot) {
    *slot_ = value;
  }
  ~ScopedFlag() { *slot_ = saved_; }

  ScopedFlag(const ScopedFlag&) = delete;
  ScopedFlag& operator=(const ScopedFlag&) = delete;

 private:
  bool* slot_;
  bool saved_;
};

class ScopedDepth {
 public:
  explicit ScopedDepth(uint32_t* depth) : depth_(depth) { ++*depth_; }
  ~ScopedDepth() { --*depth_; }

  ScopedDepth(const ScopedDepth&) = delete;
  ScopedDepth& operator=(const ScopedDepth&) = delete;

 private:
  uint32_t* depth_;
};

// ------------------------------------------------------------- reporting ----

bool Parser::Report(std::string_view code,
                    std::string message,
                    SourceSpan span,
                    std::string label) {
  if (errors_ >= options_.error_limit) {
    return false;
  }
  ++errors_;
  diagnostics_->Error(code, std::move(message))
      .Primary(span, std::move(label));
  return true;
}

std::string Parser::DescribeCurrent() const {
  const Token& token = Peek();
  if (token.kind == TokenKind::kEndOfFile) {
    return "end of file";
  }
  std::string description = "`";
  description.append(TextOf(token));
  description.push_back('`');
  return description;
}

bool Parser::ExpectToken(TokenKind kind) {
  if (Eat(kind)) {
    return true;
  }
  std::string message = "expected `";
  message.append(TokenKindSpelling(kind));
  message.append("`, found ");
  message.append(DescribeCurrent());
  Report(kExpectedToken, std::move(message), CurrentSpan());
  return false;
}

Symbol Parser::ExpectIdentifier(SourceSpan* span) {
  if (span != nullptr) {
    *span = CurrentSpan();
  }
  if (At(TokenKind::kIdentifier)) {
    return tokens_.IdentifierOf(Advance());
  }
  std::string message = "expected an identifier, found ";
  message.append(DescribeCurrent());
  Report(kExpectedIdentifier, std::move(message), CurrentSpan());
  return Symbol{};
}

bool Parser::ExpectCloseBrace(SourceSpan open) {
  if (Eat(TokenKind::kRightBrace)) {
    return true;
  }
  if (errors_ < options_.error_limit) {
    ++errors_;
    diagnostics_->Error(kUnmatchedDelimiter, "unmatched `{`")
        .Primary(CurrentSpan(), "expected `}` here")
        .Secondary(open, "unclosed `{`");
  }
  return false;
}

void Parser::ExpectStatementEnd() {
  if (Eat(TokenKind::kSemicolon)) {
    return;
  }
  if (At(TokenKind::kRightBrace) || AtEnd() || Peek().starts_line()) {
    return;
  }
  std::string message = "expected a line break before ";
  message.append(DescribeCurrent());
  Report(kExpectedStatementEnd, std::move(message), CurrentSpan(),
         "statements are separated by line breaks");
  SkipToStatementBoundary();
}

void Parser::SkipToItemBoundary() {
  while (!AtEnd()) {
    if (At(TokenKind::kRightBrace)) {
      return;
    }
    if (Peek().starts_line() && StartsItem(PeekKind())) {
      return;
    }
    Advance();
  }
}

void Parser::SkipToStatementBoundary() {
  while (!AtEnd()) {
    if (At(TokenKind::kRightBrace)) {
      return;
    }
    if (Eat(TokenKind::kSemicolon)) {
      return;
    }
    if (Peek().starts_line()) {
      return;
    }
    Advance();
  }
}

void Parser::SkipToMachineBoundary() {
  // A machine body holds only `state` declarations, so recovery skips whole
  // balanced constructs instead of stopping at the first keyword that merely
  // looks like a declaration.
  int depth = 0;
  while (!AtEnd()) {
    const TokenKind kind = PeekKind();
    if (kind == TokenKind::kLeftBrace) {
      ++depth;
      Advance();
      continue;
    }
    if (kind == TokenKind::kRightBrace) {
      if (depth == 0) {
        return;
      }
      --depth;
      Advance();
      continue;
    }
    if (depth == 0 && Peek().starts_line() &&
        AtContextual(ContextualKeyword::kState)) {
      return;
    }
    Advance();
  }
}

void Parser::SkipBalancedTo(TokenKind closing) {
  int depth = 0;
  while (!AtEnd()) {
    const TokenKind kind = PeekKind();
    if (kind == TokenKind::kLeftBrace || kind == TokenKind::kLeftParen ||
        kind == TokenKind::kLeftBracket) {
      ++depth;
    } else if (kind == closing && depth == 0) {
      Advance();
      return;
    } else if (kind == TokenKind::kRightBrace ||
               kind == TokenKind::kRightParen ||
               kind == TokenKind::kRightBracket) {
      if (depth == 0) {
        return;
      }
      --depth;
    }
    Advance();
  }
}

// ----------------------------------------------------------------- file -----

void Parser::ParseFile() {
  std::vector<ItemId> items;
  while (!AtEnd()) {
    const size_t before = pos_;
    const ItemId item = ParseItem(/*allow_error_item=*/true);
    if (item.is_valid()) {
      items.push_back(item);
    }
    if (pos_ == before) {
      // A token the lexer already complained about needs no second report.
      if (!Peek().has_error()) {
        std::string message = "expected a declaration, found ";
        message.append(DescribeCurrent());
        Report(kExpectedDeclaration, std::move(message), CurrentSpan());
      }
      if (At(TokenKind::kRightBrace)) {
        Advance();
      }
      SkipToItemBoundary();
      if (pos_ == before) {
        Advance();
      }
    }
  }
  ast_->set_root(ast_->AddRange(std::span<const ItemId>(items)));
}

ExprId Parser::ParseWholeExpression() {
  const ExprId expr = ParseExpression();
  if (!AtEnd()) {
    std::string message = "unexpected ";
    message.append(DescribeCurrent());
    message.append(" after the expression");
    Report(kTrailingTokens, std::move(message), CurrentSpan());
  }
  return expr;
}

// ---------------------------------------------------------------- items -----

SourceSpan Parser::CollectDocComments() {
  SourceSpan doc;
  while (At(TokenKind::kDocComment)) {
    const SourceSpan span = SpanOf(Advance());
    doc = doc.file.is_valid() ? doc.Merge(span) : span;
  }
  return doc;
}

void Parser::ParseAttributes(std::vector<AttributeId>* out) {
  while (At(TokenKind::kAt)) {
    const size_t start = pos_;
    Advance();

    Attribute attribute;
    attribute.name = ExpectIdentifier();
    if (At(TokenKind::kLeftParen)) {
      attribute.args = ParseCallArguments();
    }
    attribute.span = SpanFrom(start);
    out->push_back(ast_->Add(attribute));

    // A missing name means the `@` was not an attribute at all; reporting one
    // diagnostic per following `@` would be pure noise.
    if (!attribute.name.is_valid()) {
      return;
    }
  }
}

ItemFlags Parser::ParseModifiers(Symbol* abi) {
  ItemFlags flags = ItemFlags::kNone;
  const auto add = [&](ItemFlags flag, std::string_view spelling) {
    if (HasFlag(flags, flag)) {
      std::string message = "duplicate `";
      message.append(spelling);
      message.append("`");
      Report(kDuplicateModifier, std::move(message), CurrentSpan());
    }
    flags |= flag;
  };

  for (;;) {
    switch (PeekKind()) {
      case TokenKind::kKwOffer:
        add(ItemFlags::kOffer, "offer");
        Advance();
        continue;
      case TokenKind::kKwPriv:
        add(ItemFlags::kPriv, "priv");
        Advance();
        continue;
      case TokenKind::kKwTask:
        add(ItemFlags::kAsync, "task");
        Advance();
        continue;
      case TokenKind::kKwVirtual:
        add(ItemFlags::kVirtual, "virtual");
        Advance();
        continue;
      case TokenKind::kKwOverride:
        add(ItemFlags::kOverride, "override");
        Advance();
        continue;
      case TokenKind::kKwComptime:
        add(ItemFlags::kComptime, "comptime");
        Advance();
        continue;
      case TokenKind::kKwExtern:
        add(ItemFlags::kExtern, "extern");
        Advance();
        if (At(TokenKind::kStringLiteral)) {
          const Token& token = Advance();
          *abi = interner_->Intern(tokens_.StringOf(token).simple_text());
        }
        continue;
      default:
        break;
    }
    // `async fun` is what most other languages call this, and `async` is an
    // ordinary identifier here because it is also the name of an effect. A
    // reader who writes it gets told the word rather than a parse failure.
    if (At(TokenKind::kIdentifier) && PeekKind(1) == TokenKind::kKwFun &&
        TextOf(Peek()) == "async") {
      Report(kExpectedDeclaration,
             "`async fun` is spelled `task fun`; suspension is `wait`",
             CurrentSpan(), "write `task`");
      Advance();
      add(ItemFlags::kAsync, "task");
      continue;
    }

    // `derived` is contextual: only a `derived var` or `derived let` counts.
    if (AtContextual(ContextualKeyword::kDerived) &&
        (PeekKind(1) == TokenKind::kKwVar ||
         PeekKind(1) == TokenKind::kKwConst)) {
      add(ItemFlags::kDerived, "derived");
      Advance();
      continue;
    }
    break;
  }
  return flags;
}

ItemId Parser::ParseItem(bool allow_error_item) {
  const size_t start = pos_;
  const uint32_t errors_before = errors_;

  Item item;
  item.doc = CollectDocComments();

  std::vector<AttributeId> attributes;
  ParseAttributes(&attributes);

  Symbol abi;
  item.flags = ParseModifiers(&abi);
  item.abi = abi;
  item.attributes = ast_->AddRange(std::span<const AttributeId>(attributes));

  const bool had_prefix = pos_ != start;

  switch (PeekKind()) {
    case TokenKind::kKwNeed:
      return ParseImport(std::move(item), start);
    case TokenKind::kKwFun:
      return ParseFunction(std::move(item), start);
    case TokenKind::kKwClass:
      return ParseAggregate(std::move(item), start, ItemKind::kClass);
    case TokenKind::kKwStruct:
      return ParseAggregate(std::move(item), start, ItemKind::kStruct);
    case TokenKind::kKwTrait:
      return ParseAggregate(std::move(item), start, ItemKind::kTrait);
    case TokenKind::kKwComponent:
      return ParseAggregate(std::move(item), start, ItemKind::kComponent);
    case TokenKind::kKwEnum:
      return ParseEnum(std::move(item), start);
    case TokenKind::kKwExtend:
      return ParseExtend(std::move(item), start);
    case TokenKind::kKwTag:
      return ParseTag(std::move(item), start);
    case TokenKind::kKwSystem:
      return ParseSystem(std::move(item), start);
    case TokenKind::kKwSignal:
      return ParseSignal(std::move(item), start);
    case TokenKind::kKwOn:
      return ParseOn(std::move(item), start);
    case TokenKind::kKwConst:
    case TokenKind::kKwVar:
      return ParseBinding(std::move(item), start);
    case TokenKind::kKwType:
      return ParseTypeAlias(std::move(item), start);
    case TokenKind::kKwError:
      return ParseErrorSet(std::move(item), start);
    default:
      break;
  }

  // Contextual declarations. Each requires a name after the keyword, which is
  // what keeps `state`, `unit` and friends usable as ordinary identifiers.
  if (PeekKind(1) == TokenKind::kIdentifier) {
    if (AtContextual(ContextualKeyword::kDimension)) {
      return ParseDimension(std::move(item), start);
    }
    if (AtContextual(ContextualKeyword::kUnit)) {
      return ParseUnit(std::move(item), start);
    }
    if (AtContextual(ContextualKeyword::kEffect)) {
      return ParseEffect(std::move(item), start);
    }
    if (AtContextual(ContextualKeyword::kMachine)) {
      return ParseMachine(std::move(item), start);
    }
  }

  if (!had_prefix || !allow_error_item) {
    pos_ = start;
    return ItemId{};
  }

  // Stay quiet when the attributes or modifiers already produced a
  // diagnostic: the reader needs one message about this spot, not two.
  if (errors_ == errors_before && !Peek().has_error()) {
    std::string message = "expected a declaration after this, found ";
    message.append(DescribeCurrent());
    Report(kExpectedDeclaration, std::move(message), CurrentSpan());
  }
  item.kind = ItemKind::kError;
  item.flags |= ItemFlags::kHasError;
  item.span = SpanFrom(start);
  SkipToItemBoundary();
  return ast_->Add(std::move(item));
}

AstRange<ItemId> Parser::ParseItemBlock(SourceSpan open) {
  std::vector<ItemId> members;
  while (!AtEnd() && !At(TokenKind::kRightBrace)) {
    const size_t before = pos_;
    const ItemId member = ParseItem(/*allow_error_item=*/true);
    if (member.is_valid()) {
      members.push_back(member);
    }
    if (pos_ == before) {
      std::string message = "expected a member declaration, found ";
      message.append(DescribeCurrent());
      Report(kExpectedDeclaration, std::move(message), CurrentSpan());
      SkipToItemBoundary();
      if (pos_ == before) {
        Advance();
      }
    }
  }
  ExpectCloseBrace(open);
  return ast_->AddRange(std::span<const ItemId>(members));
}

ItemId Parser::ParseImport(Item item, size_t start) {
  Advance();  // `need`
  item.kind = ItemKind::kNeed;

  std::vector<Symbol> segments;
  std::vector<Symbol> names;
  for (;;) {
    if (At(TokenKind::kLeftBrace)) {
      const SourceSpan open = CurrentSpan();
      Advance();
      item.flags |= ItemFlags::kNeedList;
      while (!AtEnd() && !At(TokenKind::kRightBrace)) {
        const Symbol name = ExpectIdentifier();
        if (name.is_valid()) {
          names.push_back(name);
        } else {
          break;
        }
        if (!Eat(TokenKind::kComma)) {
          break;
        }
      }
      ExpectCloseBrace(open);
      break;
    }

    const Symbol segment = ExpectIdentifier();
    if (!segment.is_valid()) {
      item.flags |= ItemFlags::kHasError;
      break;
    }
    segments.push_back(segment);
    if (!Eat(TokenKind::kDot)) {
      break;
    }
  }

  if (!segments.empty()) {
    item.name = segments.back();
  }
  item.path = ast_->AddRange(std::span<const Symbol>(segments));
  item.names = ast_->AddRange(std::span<const Symbol>(names));
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

AstRange<ParamId> Parser::ParseParameterList(bool types_required) {
  std::vector<ParamId> params;
  if (!ExpectToken(TokenKind::kLeftParen)) {
    return ast_->AddRange(std::span<const ParamId>(params));
  }

  while (!AtEnd() && !At(TokenKind::kRightParen)) {
    const size_t start = pos_;
    Param param;

    std::vector<AttributeId> attributes;
    ParseAttributes(&attributes);
    param.attributes =
        ast_->AddRange(std::span<const AttributeId>(attributes));

    param.is_mutable = Eat(TokenKind::kKwMut);
    param.name = ExpectIdentifier();
    if (!param.name.is_valid()) {
      SkipBalancedTo(TokenKind::kRightParen);
      break;
    }

    if (Eat(TokenKind::kColon)) {
      param.type = ParseType();
    } else if (types_required) {
      std::string message = "expected `:` and a parameter type, found ";
      message.append(DescribeCurrent());
      Report(kExpectedType, std::move(message), CurrentSpan());
    }

    if (Eat(TokenKind::kEq)) {
      param.default_value = ParseExpression();
    }

    param.span = SpanFrom(start);
    params.push_back(ast_->Add(param));

    if (!Eat(TokenKind::kComma)) {
      break;
    }
  }

  ExpectToken(TokenKind::kRightParen);
  return ast_->AddRange(std::span<const ParamId>(params));
}

AstRange<GenericParamId> Parser::ParseGenericParameters() {
  std::vector<GenericParamId> generics;
  if (!At(TokenKind::kLess)) {
    return ast_->AddRange(std::span<const GenericParamId>(generics));
  }
  Advance();

  while (!AtEnd() && !AtAngleClose()) {
    const size_t start = pos_;
    GenericParam param;
    param.name = ExpectIdentifier();
    if (!param.name.is_valid()) {
      break;
    }

    std::vector<TypeRefId> bounds;
    if (Eat(TokenKind::kColon)) {
      for (;;) {
        bounds.push_back(ParseType());
        if (!Eat(TokenKind::kPlus)) {
          break;
        }
      }
    }
    param.bounds = ast_->AddRange(std::span<const TypeRefId>(bounds));
    param.span = SpanFrom(start);
    generics.push_back(ast_->Add(param));

    if (!Eat(TokenKind::kComma)) {
      break;
    }
  }

  if (!EatAngleClose()) {
    ExpectToken(TokenKind::kGreater);
  }
  return ast_->AddRange(std::span<const GenericParamId>(generics));
}

void Parser::ParseWhereClause(std::vector<GenericParamId>* generics) {
  if (!At(TokenKind::kKwWhere)) {
    return;
  }
  Advance();

  for (;;) {
    const SourceSpan name_span = CurrentSpan();
    const Symbol name = ExpectIdentifier();
    if (!name.is_valid()) {
      return;
    }

    std::vector<TypeRefId> bounds;
    if (ExpectToken(TokenKind::kColon)) {
      for (;;) {
        bounds.push_back(ParseType());
        if (!Eat(TokenKind::kPlus)) {
          break;
        }
      }
    }

    GenericParamId target;
    for (GenericParamId id : *generics) {
      if (ast_->Get(id).name == name) {
        target = id;
        break;
      }
    }

    if (!target.is_valid()) {
      std::string message = "`";
      message.append(interner_->Text(name));
      message.append("` is not a type parameter of this declaration");
      Report(kUnknownTypeParameter, std::move(message), name_span);
    } else {
      // Merge the new bounds with the ones written inline.
      GenericParam& param = ast_->GetMutable(target);
      std::vector<TypeRefId> merged;
      for (TypeRefId bound : ast_->Get(param.bounds)) {
        merged.push_back(bound);
      }
      for (TypeRefId bound : bounds) {
        merged.push_back(bound);
      }
      param.bounds = ast_->AddRange(std::span<const TypeRefId>(merged));
    }

    if (!Eat(TokenKind::kComma)) {
      return;
    }
  }
}

AstRange<TypeRefId> Parser::ParseEffectList() {
  std::vector<TypeRefId> effects;
  if (!Eat(TokenKind::kKwUses)) {
    return ast_->AddRange(std::span<const TypeRefId>(effects));
  }
  for (;;) {
    // `unsafe` is a keyword everywhere else, but it is also one of the
    // built-in effects, so an effect list accepts the spelling.
    if (Peek().kind == TokenKind::kKwUnsafe) {
      const Token& token = Advance();
      TypeRef effect;
      effect.kind = TypeRefKind::kPath;
      effect.span = SpanOf(token);
      const Symbol name = interner_->Intern("unsafe");
      effect.path = ast_->AddRange(std::span<const Symbol>(&name, 1));
      effects.push_back(ast_->Add(std::move(effect)));
    } else {
      effects.push_back(ParseType());
    }
    if (!Eat(TokenKind::kComma)) {
      break;
    }
  }
  return ast_->AddRange(std::span<const TypeRefId>(effects));
}

ItemId Parser::ParseFunction(Item item, size_t start) {
  Advance();  // `fun`
  item.kind = ItemKind::kFunction;
  item.name = ExpectIdentifier(&item.name_span);

  std::vector<GenericParamId> generics;
  for (GenericParamId id : ast_->Get(ParseGenericParameters())) {
    generics.push_back(id);
  }

  item.params = ParseParameterList(/*types_required=*/true);

  if (Eat(TokenKind::kArrow)) {
    item.type = ParseType();
  }
  item.effects = ParseEffectList();
  ParseWhereClause(&generics);
  item.generics = ast_->AddRange(std::span<const GenericParamId>(generics));

  if (At(TokenKind::kLeftBrace)) {
    item.body = ParseBlock();
  } else if (Eat(TokenKind::kFatArrow)) {
    item.flags |= ItemFlags::kExpressionBody;
    item.body = ParseExpression();
  } else if (Eat(TokenKind::kEq)) {
    // `extern "C" fn f() = "symbol"`
    if (At(TokenKind::kStringLiteral)) {
      const Token& token = Advance();
      item.link_name =
          interner_->Intern(tokens_.StringOf(token).simple_text());
    } else {
      Report(kExpectedToken, "expected the linker symbol as a string literal",
             CurrentSpan());
    }
  } else if (!Peek().starts_line() && !At(TokenKind::kRightBrace) &&
             !AtEnd()) {
    std::string message = "expected a function body, found ";
    message.append(DescribeCurrent());
    Report(kExpectedBlock, std::move(message), CurrentSpan(),
           "write `{ ... }`, `=> expr`, or nothing for a declaration");
  }

  if (item.body.is_valid()) {
    item.flags |= ItemFlags::kHasBody;
  }
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseAggregate(Item item, size_t start, ItemKind kind) {
  Advance();  // `class`, `struct`, `trait` or `component`
  item.kind = kind;
  item.name = ExpectIdentifier(&item.name_span);

  std::vector<GenericParamId> generics;
  for (GenericParamId id : ast_->Get(ParseGenericParameters())) {
    generics.push_back(id);
  }

  if (Eat(TokenKind::kColon)) {
    item.super_type = ParseType();
  }
  ParseWhereClause(&generics);
  item.generics = ast_->AddRange(std::span<const GenericParamId>(generics));

  const SourceSpan open = CurrentSpan();
  if (ExpectToken(TokenKind::kLeftBrace)) {
    item.members = ParseItemBlock(open);
  } else {
    item.flags |= ItemFlags::kHasError;
  }

  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseEnum(Item item, size_t start) {
  Advance();  // `enum`
  item.kind = ItemKind::kEnum;
  item.name = ExpectIdentifier(&item.name_span);
  item.generics = ParseGenericParameters();

  const SourceSpan open = CurrentSpan();
  if (!ExpectToken(TokenKind::kLeftBrace)) {
    item.flags |= ItemFlags::kHasError;
    item.span = SpanFrom(start);
    return ast_->Add(std::move(item));
  }

  std::vector<ItemId> variants;
  while (!AtEnd() && !At(TokenKind::kRightBrace)) {
    const size_t variant_start = pos_;
    Item variant;
    variant.kind = ItemKind::kVariant;
    variant.doc = CollectDocComments();

    std::vector<AttributeId> attributes;
    ParseAttributes(&attributes);
    variant.attributes =
        ast_->AddRange(std::span<const AttributeId>(attributes));

    variant.name = ExpectIdentifier(&variant.name_span);
    if (!variant.name.is_valid()) {
      SkipToItemBoundary();
      break;
    }
    if (At(TokenKind::kLeftParen)) {
      variant.params = ParseParameterList(/*types_required=*/true);
    }
    variant.span = SpanFrom(variant_start);
    variants.push_back(ast_->Add(std::move(variant)));

    Eat(TokenKind::kComma);
  }

  ExpectCloseBrace(open);
  item.members = ast_->AddRange(std::span<const ItemId>(variants));
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseExtend(Item item, size_t start) {
  Advance();  // `extend`
  item.kind = ItemKind::kExtend;

  std::vector<GenericParamId> generics;
  for (GenericParamId id : ast_->Get(ParseGenericParameters())) {
    generics.push_back(id);
  }

  // `extend T` adds inherent members; `extend T as Trait` implements a trait
  // for it. The target comes first either way, which is what makes the form
  // read as a sentence.
  item.target_type = ParseType();
  if (Eat(TokenKind::kKwAs)) {
    item.flags |= ItemFlags::kTraitImpl;
    item.super_type = ParseType();
  }

  if (item.target_type.is_valid()) {
    const TypeRef& target = ast_->Get(item.target_type);
    const std::span<const Symbol> path = ast_->Get(target.path);
    if (!path.empty()) {
      item.name = path.back();
    }
  }

  ParseWhereClause(&generics);
  item.generics = ast_->AddRange(std::span<const GenericParamId>(generics));

  const SourceSpan open = CurrentSpan();
  if (ExpectToken(TokenKind::kLeftBrace)) {
    item.members = ParseItemBlock(open);
  } else {
    item.flags |= ItemFlags::kHasError;
  }

  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseTag(Item item, size_t start) {
  Advance();  // `tag`
  item.kind = ItemKind::kTag;
  item.name = ExpectIdentifier(&item.name_span);
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseSystem(Item item, size_t start) {
  Advance();  // `system`
  item.kind = ItemKind::kSystem;
  item.name = ExpectIdentifier(&item.name_span);
  item.params = ParseParameterList(/*types_required=*/true);
  item.effects = ParseEffectList();

  std::vector<OrderingId> orderings;
  for (;;) {
    const bool is_after = At(TokenKind::kKwAfter);
    if (!is_after && !At(TokenKind::kKwBefore)) {
      break;
    }
    const size_t ordering_start = pos_;
    Advance();

    Ordering ordering;
    ordering.is_after = is_after;
    ordering.path = ParseDottedPath();
    ordering.span = SpanFrom(ordering_start);
    orderings.push_back(ast_->Add(ordering));

    // `after A, B` and `after A before C` are both allowed.
    Eat(TokenKind::kComma);
  }
  item.orderings = ast_->AddRange(std::span<const OrderingId>(orderings));

  if (At(TokenKind::kLeftBrace)) {
    item.body = ParseBlock();
  } else {
    Report(kExpectedBlock, "expected a system body", CurrentSpan());
    item.flags |= ItemFlags::kHasError;
  }

  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseSignal(Item item, size_t start) {
  Advance();  // `signal`
  item.kind = ItemKind::kSignal;
  item.name = ExpectIdentifier(&item.name_span);
  item.params = ParseParameterList(/*types_required=*/true);
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseOn(Item item, size_t start) {
  Advance();  // `on`
  item.kind = ItemKind::kOn;
  item.path = ParseDottedPath();
  const std::span<const Symbol> path = ast_->Get(item.path);
  if (!path.empty()) {
    item.name = path.back();
  }

  // Handler parameters may omit types, because the signal declares them.
  item.params = ParseParameterList(/*types_required=*/false);

  if (At(TokenKind::kLeftBrace)) {
    item.body = ParseBlock();
  } else {
    Report(kExpectedBlock, "expected a handler body", CurrentSpan());
    item.flags |= ItemFlags::kHasError;
  }

  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseBinding(Item item, size_t start) {
  item.kind = ItemKind::kBinding;
  if (At(TokenKind::kKwVar)) {
    item.flags |= ItemFlags::kMutable;
  }
  Advance();  // `let` or `var`

  item.name = ExpectIdentifier(&item.name_span);
  if (Eat(TokenKind::kColon)) {
    item.type = ParseType();
  }
  if (Eat(TokenKind::kFatArrow)) {
    item.flags |= ItemFlags::kExpressionBody;
    item.init = ParseExpression();
  } else if (Eat(TokenKind::kEq)) {
    item.init = ParseExpression();
  }

  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseTypeAlias(Item item, size_t start) {
  Advance();  // `type`
  item.kind = ItemKind::kTypeAlias;
  item.name = ExpectIdentifier(&item.name_span);
  item.generics = ParseGenericParameters();
  if (ExpectToken(TokenKind::kEq)) {
    item.type = ParseType();
  }
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseDimension(Item item, size_t start) {
  Advance();  // `dimension`
  item.kind = ItemKind::kDimension;

  std::vector<Symbol> names;
  for (;;) {
    SourceSpan name_span;
    const Symbol name = ExpectIdentifier(&name_span);
    if (!name.is_valid()) {
      break;
    }
    if (names.empty()) {
      item.name_span = name_span;
    }
    names.push_back(name);

    // `dimension Velocity = Length / Time` names a derived dimension. The
    // product is confined to this declaration rather than being allowed in
    // general type syntax, so a velocity is named once and used by name.
    if (names.size() == 1 && Eat(TokenKind::kEq)) {
      item.init = ParseExpression();
      break;
    }
    if (!Eat(TokenKind::kComma)) {
      break;
    }
  }
  if (!names.empty()) {
    item.name = names.front();
  }
  item.names = ast_->AddRange(std::span<const Symbol>(names));
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseUnit(Item item, size_t start) {
  Advance();  // `unit`
  item.kind = ItemKind::kUnit;
  item.name = ExpectIdentifier(&item.name_span);

  if (Eat(TokenKind::kColon)) {
    item.path = ParseDottedPath();
  } else if (Eat(TokenKind::kEq)) {
    item.init = ParseExpression();
  } else {
    std::string message = "expected `: Dimension` or `= factor`, found ";
    message.append(DescribeCurrent());
    Report(kExpectedUnitDefinition, std::move(message), CurrentSpan(),
           "a unit either names its dimension or converts to another unit");
    item.flags |= ItemFlags::kHasError;
  }

  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseErrorSet(Item item, size_t start) {
  Advance();  // `error`
  item.kind = ItemKind::kErrorSet;

  std::vector<Symbol> names;
  for (;;) {
    SourceSpan name_span;
    const Symbol name = ExpectIdentifier(&name_span);
    if (!name.is_valid()) {
      break;
    }
    if (names.empty()) {
      item.name = name;
      item.name_span = name_span;
    }
    names.push_back(name);
    if (!Eat(TokenKind::kComma)) {
      break;
    }
  }
  item.names = ast_->AddRange(std::span<const Symbol>(names));
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseEffect(Item item, size_t start) {
  Advance();  // `effect`
  item.kind = ItemKind::kEffect;
  item.name = ExpectIdentifier(&item.name_span);
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

ItemId Parser::ParseMachine(Item item, size_t start) {
  Advance();  // `machine`
  item.kind = ItemKind::kMachine;
  item.name = ExpectIdentifier(&item.name_span);

  const SourceSpan open = CurrentSpan();
  if (!ExpectToken(TokenKind::kLeftBrace)) {
    item.flags |= ItemFlags::kHasError;
    item.span = SpanFrom(start);
    return ast_->Add(std::move(item));
  }

  std::vector<StateId> states;
  while (!AtEnd() && !At(TokenKind::kRightBrace)) {
    const size_t before = pos_;
    if (AtContextual(ContextualKeyword::kState)) {
      states.push_back(ParseState());
    } else {
      std::string message = "expected a `state` declaration, found ";
      message.append(DescribeCurrent());
      Report(kExpectedState, std::move(message), CurrentSpan());
      item.flags |= ItemFlags::kHasError;
      SkipToMachineBoundary();
    }
    if (pos_ == before) {
      Advance();
    }
  }

  ExpectCloseBrace(open);
  item.states = ast_->AddRange(std::span<const StateId>(states));
  item.span = SpanFrom(start);
  return ast_->Add(std::move(item));
}

StateId Parser::ParseState() {
  const size_t start = pos_;
  Advance();  // `state`

  State state;
  state.name = ExpectIdentifier();

  const SourceSpan open = CurrentSpan();
  if (!ExpectToken(TokenKind::kLeftBrace)) {
    state.span = SpanFrom(start);
    return ast_->Add(state);
  }

  std::vector<TransitionId> transitions;
  while (!AtEnd() && !At(TokenKind::kRightBrace)) {
    const size_t before = pos_;
    if (AtContextual(ContextualKeyword::kEnter)) {
      Advance();
      state.enter = ParseBlock();
    } else if (AtContextual(ContextualKeyword::kExit)) {
      Advance();
      state.exit = ParseBlock();
    } else if (!ParseTransition(&transitions)) {
      SkipToStatementBoundary();
    }
    if (pos_ == before) {
      Advance();
    }
  }

  ExpectCloseBrace(open);
  state.transitions =
      ast_->AddRange(std::span<const TransitionId>(transitions));
  state.span = SpanFrom(start);
  return ast_->Add(state);
}

bool Parser::ParseTransition(std::vector<TransitionId>* out) {
  const size_t start = pos_;
  Transition transition;

  if (Eat(TokenKind::kKwOn)) {
    transition.event = ExpectIdentifier();
  } else if (AtContextual(ContextualKeyword::kAlways)) {
    Advance();
    transition.is_always = true;
  } else {
    std::string message = "expected `on`, `always`, `enter` or `exit`, found ";
    message.append(DescribeCurrent());
    Report(kExpectedTransition, std::move(message), CurrentSpan());
    return false;
  }

  if (Eat(TokenKind::kKwAfter)) {
    transition.delay = ParseExpression();
  }
  if (!ExpectToken(TokenKind::kArrow)) {
    return false;
  }
  transition.target = ExpectIdentifier();
  transition.span = SpanFrom(start);
  out->push_back(ast_->Add(transition));
  return true;
}

// ----------------------------------------------------------- statements -----

ExprId Parser::ParseBlock() {
  const size_t start = pos_;
  const SourceSpan open = CurrentSpan();
  if (!ExpectToken(TokenKind::kLeftBrace)) {
    return MakeError(start);
  }

  // A block body is a fresh context: `{` inside it opens a nested block or a
  // struct literal again, even when the block is a loop body.
  ScopedFlag allow(&allow_struct_literal_, true);

  std::vector<StmtId> stmts;
  while (!AtEnd() && !At(TokenKind::kRightBrace)) {
    const size_t before = pos_;
    const StmtId stmt = ParseStatement();
    if (stmt.is_valid()) {
      stmts.push_back(stmt);
    }
    if (pos_ == before) {
      Advance();
    }
  }
  ExpectCloseBrace(open);

  Expr expr;
  expr.kind = ExprKind::kBlock;
  expr.stmts = ast_->AddRange(std::span<const StmtId>(stmts));
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

StmtId Parser::ParseStatement() {
  const size_t start = pos_;
  Stmt stmt;

  switch (PeekKind()) {
    case TokenKind::kSemicolon:
      Advance();
      return StmtId{};

    case TokenKind::kKwConst:
    case TokenKind::kKwVar: {
      stmt.kind = At(TokenKind::kKwConst) ? StmtKind::kLet : StmtKind::kVar;
      Advance();
      stmt.pattern = ParsePattern();
      if (Eat(TokenKind::kColon)) {
        stmt.type = ParseType();
      }
      if (Eat(TokenKind::kEq)) {
        stmt.expr = ParseExpression();
      }
      stmt.span = SpanFrom(start);
      ExpectStatementEnd();
      return ast_->Add(stmt);
    }

    case TokenKind::kKwReturn: {
      stmt.kind = StmtKind::kReturn;
      Advance();
      if (!At(TokenKind::kRightBrace) && !AtEnd() &&
          !At(TokenKind::kSemicolon) && !Peek().starts_line()) {
        stmt.expr = ParseExpression();
      }
      stmt.span = SpanFrom(start);
      ExpectStatementEnd();
      return ast_->Add(stmt);
    }

    case TokenKind::kKwBreak:
    case TokenKind::kKwContinue: {
      stmt.kind = At(TokenKind::kKwBreak) ? StmtKind::kBreak
                                          : StmtKind::kContinue;
      Advance();
      // A label must be on the same line, so a following statement that
      // happens to start with an identifier is not swallowed.
      if (At(TokenKind::kIdentifier) && !Peek().starts_line()) {
        const SourceSpan label_span = CurrentSpan();
        stmt.label = tokens_.IdentifierOf(Advance());
        bool known = false;
        for (Symbol label : labels_) {
          if (label == stmt.label) {
            known = true;
            break;
          }
        }
        if (!known) {
          std::string message = "no enclosing loop is labelled `";
          message.append(interner_->Text(stmt.label));
          message.append("`");
          Report(kUnknownLabel, std::move(message), label_span);
        }
      }
      stmt.span = SpanFrom(start);
      ExpectStatementEnd();
      return ast_->Add(stmt);
    }

    case TokenKind::kKwDefer: {
      stmt.kind = StmtKind::kDefer;
      Advance();
      stmt.expr = At(TokenKind::kLeftBrace) ? ParseBlock() : ParseExpression();
      stmt.span = SpanFrom(start);
      ExpectStatementEnd();
      return ast_->Add(stmt);
    }

    default:
      break;
  }

  // Nested declarations.
  if (StartsItem(PeekKind()) ||
      (PeekKind(1) == TokenKind::kIdentifier &&
       (AtContextual(ContextualKeyword::kMachine) ||
        AtContextual(ContextualKeyword::kDimension) ||
        AtContextual(ContextualKeyword::kUnit) ||
        AtContextual(ContextualKeyword::kEffect)))) {
    const ItemId item = ParseItem(/*allow_error_item=*/false);
    if (item.is_valid()) {
      stmt.kind = StmtKind::kItem;
      stmt.item = item;
      stmt.span = SpanFrom(start);
      return ast_->Add(stmt);
    }
  }

  stmt.kind = StmtKind::kExpr;
  stmt.expr = ParseExpression();
  stmt.span = SpanFrom(start);
  if (!ast_->Contains(stmt.expr) ||
      ast_->Get(stmt.expr).kind == ExprKind::kError) {
    stmt.flags |= StmtFlags::kHasError;
  }
  ExpectStatementEnd();
  return ast_->Add(stmt);
}

// ---------------------------------------------------------- expressions -----

ExprId Parser::MakeError(size_t start) {
  Expr expr;
  expr.kind = ExprKind::kError;
  expr.flags |= ExprFlags::kHasError;
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

ExprId Parser::ParseExpression(Precedence min_precedence) {
  ScopedDepth depth(&depth_);
  if (depth_ > kMaxNesting) {
    const size_t start = pos_;
    Report(kNestingTooDeep, "expression nests too deeply", CurrentSpan());
    while (!AtEnd() && !At(TokenKind::kRightBrace) &&
           !At(TokenKind::kSemicolon)) {
      Advance();
    }
    return MakeError(start);
  }

  const size_t start = pos_;

  // A range with no lower bound: `..b` and a bare `..`.
  if (At(TokenKind::kDotDot) || At(TokenKind::kDotDotEq)) {
    const TokenKind op = PeekKind();
    Advance();
    Expr range;
    range.kind = ExprKind::kRange;
    range.op = op;
    if (op == TokenKind::kDotDotEq) {
      range.flags |= ExprFlags::kInclusive;
    }
    if (StartsExpression(PeekKind(), allow_struct_literal_)) {
      range.rhs = ParseExpression(Tighter(Precedence::kRange));
    }
    range.span = SpanFrom(start);
    return ast_->Add(std::move(range));
  }

  ExprId left = ParseUnary();

  for (;;) {
    const Token& token = Peek();
    const Precedence precedence = InfixPrecedence(token.kind);
    if (precedence == Precedence::kNone || precedence < min_precedence) {
      break;
    }
    // An operator that opens a line starts a new statement instead of
    // continuing this expression, so a line that begins with `-` is never
    // folded into the previous one. A continuation keeps the operator at the
    // end of the previous line. Leading-dot method chains are handled in
    // ParsePostfix, which exempts member access from this rule.
    if (token.starts_line()) {
      break;
    }

    const TokenKind op = token.kind;
    Advance();

    if (op == TokenKind::kKwAs) {
      Expr cast;
      cast.kind = ExprKind::kCast;
      cast.lhs = left;
      cast.type = ParseType();
      cast.span = SpanFrom(start);
      left = ast_->Add(std::move(cast));
      continue;
    }

    // A range with no upper bound: `a..` followed by anything that cannot
    // start an expression.
    if ((op == TokenKind::kDotDot || op == TokenKind::kDotDotEq) &&
        !StartsExpression(PeekKind(), allow_struct_literal_)) {
      Expr range;
      range.kind = ExprKind::kRange;
      range.op = op;
      if (op == TokenKind::kDotDotEq) {
        range.flags |= ExprFlags::kInclusive;
      }
      range.lhs = left;
      range.span = SpanFrom(start);
      left = ast_->Add(std::move(range));
      continue;
    }

    // Assignment is right associative; everything else is left associative.
    const Precedence next_precedence =
        IsAssignmentOperator(op)
            ? precedence
            : static_cast<Precedence>(static_cast<int>(precedence) + 1);
    const ExprId right = ParseExpression(next_precedence);

    Expr expr;
    if (IsAssignmentOperator(op)) {
      expr.kind = ExprKind::kAssign;
    } else if (op == TokenKind::kDotDot || op == TokenKind::kDotDotEq) {
      expr.kind = ExprKind::kRange;
      if (op == TokenKind::kDotDotEq) {
        expr.flags |= ExprFlags::kInclusive;
      }
    } else {
      expr.kind = ExprKind::kBinary;
    }
    expr.op = op;
    expr.lhs = left;
    expr.rhs = right;
    expr.span = SpanFrom(start);
    left = ast_->Add(std::move(expr));
  }

  return left;
}

ExprId Parser::ParseUnary() {
  const size_t start = pos_;
  const TokenKind kind = PeekKind();

  switch (kind) {
    case TokenKind::kMinus:
    case TokenKind::kBang:
    case TokenKind::kTilde:
    case TokenKind::kStar:
    case TokenKind::kAmp: {
      Advance();
      Expr expr;
      expr.kind = ExprKind::kUnary;
      expr.op = kind;
      if (kind == TokenKind::kAmp && Eat(TokenKind::kKwMut)) {
        expr.flags |= ExprFlags::kMutable;
      }
      expr.lhs = ParseUnary();
      expr.span = SpanFrom(start);
      return ast_->Add(std::move(expr));
    }

    case TokenKind::kKwWait:
    case TokenKind::kKwSpawn: {
      Advance();
      Expr expr;
      expr.kind = kind == TokenKind::kKwWait ? ExprKind::kAwait
                                              : ExprKind::kSpawn;
      expr.lhs = ParseUnary();
      expr.span = SpanFrom(start);
      return ast_->Add(std::move(expr));
    }

    default:
      break;
  }

  return ParsePostfix(ParsePrimary(), start);
}

ExprId Parser::ParsePostfix(ExprId operand, size_t start) {
  for (;;) {
    const Token& token = Peek();
    const TokenKind kind = token.kind;

    // A new line ends the chain, except for member access, which is how
    // multi-line method chains are written.
    if (token.starts_line() && kind != TokenKind::kDot &&
        kind != TokenKind::kQuestionDot) {
      return operand;
    }

    switch (kind) {
      case TokenKind::kLeftParen: {
        Expr call;
        call.kind = ExprKind::kCall;
        call.lhs = operand;
        call.args = ParseCallArguments();
        call.span = SpanFrom(start);
        operand = ast_->Add(std::move(call));
        continue;
      }

      case TokenKind::kLeftBracket: {
        Advance();
        Expr index;
        index.kind = ExprKind::kIndex;
        index.lhs = operand;
        {
          ScopedFlag allow(&allow_struct_literal_, true);
          index.rhs = ParseExpression();
        }
        ExpectToken(TokenKind::kRightBracket);
        index.span = SpanFrom(start);
        operand = ast_->Add(std::move(index));
        continue;
      }

      case TokenKind::kDot:
      case TokenKind::kQuestionDot: {
        Advance();
        Expr field;
        field.kind = kind == TokenKind::kDot ? ExprKind::kField
                                             : ExprKind::kOptionalField;
        field.lhs = operand;
        field.name = ExpectIdentifier();
        field.span = SpanFrom(start);
        operand = ast_->Add(std::move(field));
        continue;
      }

      case TokenKind::kQuestion: {
        Advance();
        Expr attempt;
        attempt.kind = ExprKind::kTry;
        attempt.lhs = operand;
        attempt.span = SpanFrom(start);
        operand = ast_->Add(std::move(attempt));
        continue;
      }

      case TokenKind::kLess: {
        if (!LooksLikeGenericCall()) {
          return operand;
        }
        const AstRange<TypeRefId> generic_args = ParseGenericArguments();
        if (!At(TokenKind::kLeftParen)) {
          // The guess was wrong in a way the scan could not see; report it
          // against the call that was expected.
          Report(kExpectedToken, "expected `(` after the generic arguments",
                 CurrentSpan());
          return operand;
        }
        Expr call;
        call.kind = ExprKind::kCall;
        call.flags |= ExprFlags::kHasGenericArgs;
        call.lhs = operand;
        call.generic_args = generic_args;
        call.args = ParseCallArguments();
        call.span = SpanFrom(start);
        operand = ast_->Add(std::move(call));
        continue;
      }

      case TokenKind::kLeftBrace: {
        if (!allow_struct_literal_) {
          return operand;
        }
        const ExprKind operand_kind = ast_->Contains(operand)
                                          ? ast_->Get(operand).kind
                                          : ExprKind::kError;
        // Only a name, a member path or a call result can be initialized with
        // a brace block.
        if (operand_kind != ExprKind::kPath &&
            operand_kind != ExprKind::kField &&
            operand_kind != ExprKind::kCall) {
          return operand;
        }

        const SourceSpan open = CurrentSpan();
        Advance();
        std::vector<ExprId> fields;
        {
          ScopedFlag allow(&allow_struct_literal_, true);
          while (!AtEnd() && !At(TokenKind::kRightBrace)) {
            const size_t before = pos_;
            fields.push_back(ParseExpression());
            if (!Eat(TokenKind::kComma) && !Peek().starts_line() &&
                !At(TokenKind::kRightBrace)) {
              break;
            }
            if (pos_ == before) {
              break;
            }
          }
        }
        ExpectCloseBrace(open);

        Expr literal;
        literal.kind = ExprKind::kStructLiteral;
        literal.lhs = operand;
        literal.args = ast_->AddRange(std::span<const ExprId>(fields));
        literal.span = SpanFrom(start);
        operand = ast_->Add(std::move(literal));
        continue;
      }

      default:
        return operand;
    }
  }
}

AstRange<ExprId> Parser::ParseCallArguments() {
  std::vector<ExprId> args;
  const SourceSpan open = CurrentSpan();
  if (!ExpectToken(TokenKind::kLeftParen)) {
    return ast_->AddRange(std::span<const ExprId>(args));
  }

  ScopedFlag allow(&allow_struct_literal_, true);
  while (!AtEnd() && !At(TokenKind::kRightParen)) {
    const size_t before = pos_;
    args.push_back(ParseExpression());
    if (!Eat(TokenKind::kComma)) {
      break;
    }
    if (pos_ == before) {
      break;
    }
  }

  if (!Eat(TokenKind::kRightParen)) {
    if (errors_ < options_.error_limit) {
      ++errors_;
      diagnostics_->Error(kUnmatchedDelimiter, "unmatched `(`")
          .Primary(CurrentSpan(), "expected `)` here")
          .Secondary(open, "unclosed `(`");
    }
    SkipBalancedTo(TokenKind::kRightParen);
  }
  return ast_->AddRange(std::span<const ExprId>(args));
}

bool Parser::LooksLikeGenericCall() const {
  WOLANG_DCHECK(At(TokenKind::kLess));
  int depth = 0;
  for (size_t i = pos_; i < pos_ + 64; ++i) {
    const Token& token = tokens_.At(i);
    switch (token.kind) {
      case TokenKind::kLess:
        ++depth;
        break;
      case TokenKind::kShl:
        depth += 2;
        break;
      case TokenKind::kGreater:
      case TokenKind::kShr: {
        depth -= token.kind == TokenKind::kGreater ? 1 : 2;
        if (depth > 0) {
          break;
        }
        // Only a call can follow explicit generic arguments.
        return tokens_.At(i + 1).kind == TokenKind::kLeftParen;
      }
      case TokenKind::kEndOfFile:
        return false;
      default:
        if (!CanAppearInGenericArguments(token.kind)) {
          return false;
        }
        break;
    }
  }
  return false;
}

ExprId Parser::ParseConditionWithOptionalBinding(PatternId* pattern) {
  if (Eat(TokenKind::kKwConst)) {
    // A pattern position: `{` here can only start a struct pattern, never a
    // block, so `if let Point { x } = p` parses.
    *pattern = ParsePattern();
    ExpectToken(TokenKind::kEq);
  }
  ScopedFlag allow(&allow_struct_literal_, false);
  return ParseExpression();
}

ExprId Parser::ParseIf() {
  const size_t start = pos_;
  Advance();  // `if`

  Expr expr;
  expr.kind = ExprKind::kIf;

  PatternId pattern;
  expr.lhs = ParseConditionWithOptionalBinding(&pattern);
  if (pattern.is_valid()) {
    expr.pattern = pattern;
    expr.flags |= ExprFlags::kHasBinding;
  }

  expr.rhs = ParseBlock();
  if (Eat(TokenKind::kKwElse)) {
    expr.extra = At(TokenKind::kKwIf) ? ParseIf() : ParseBlock();
  }
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

ExprId Parser::ParseMatch() {
  const size_t start = pos_;
  Advance();  // `match`

  Expr expr;
  expr.kind = ExprKind::kMatch;
  {
    ScopedFlag allow(&allow_struct_literal_, false);
    expr.lhs = ParseExpression();
  }

  const SourceSpan open = CurrentSpan();
  if (!ExpectToken(TokenKind::kLeftBrace)) {
    expr.flags |= ExprFlags::kHasError;
    expr.span = SpanFrom(start);
    return ast_->Add(std::move(expr));
  }

  std::vector<MatchArmId> arms;
  while (!AtEnd() && !At(TokenKind::kRightBrace)) {
    const size_t arm_start = pos_;
    MatchArm arm;
    arm.patterns = ParsePatternAlternatives();
    if (arm.patterns.empty()) {
      std::string message = "expected a match arm, found ";
      message.append(DescribeCurrent());
      Report(kExpectedMatchArm, std::move(message), CurrentSpan());
      SkipToStatementBoundary();
      if (pos_ == arm_start) {
        Advance();
      }
      continue;
    }

    if (Eat(TokenKind::kKwIf)) {
      ScopedFlag allow(&allow_struct_literal_, false);
      arm.guard = ParseExpression();
    }

    if (ExpectToken(TokenKind::kFatArrow)) {
      arm.body =
          At(TokenKind::kLeftBrace) ? ParseBlock() : ParseExpression();
    } else {
      arm.body = MakeError(arm_start);
      SkipToStatementBoundary();
    }

    arm.span = SpanFrom(arm_start);
    arms.push_back(ast_->Add(arm));
    Eat(TokenKind::kComma);
    if (pos_ == arm_start) {
      Advance();
    }
  }

  ExpectCloseBrace(open);
  expr.arms = ast_->AddRange(std::span<const MatchArmId>(arms));
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

ExprId Parser::ParseLoopLike(TokenKind keyword, Symbol label) {
  const size_t start = pos_;
  Advance();  // `loop`, `while`, `for` or `foreach`

  Expr expr;
  expr.label = label;
  switch (keyword) {
    case TokenKind::kKwLoop:
      expr.kind = ExprKind::kLoop;
      expr.rhs = ParseBlock();
      expr.span = SpanFrom(start);
      return ast_->Add(std::move(expr));

    case TokenKind::kKwWhile: {
      expr.kind = ExprKind::kWhile;
      PatternId pattern;
      expr.lhs = ParseConditionWithOptionalBinding(&pattern);
      if (pattern.is_valid()) {
        expr.pattern = pattern;
        expr.flags |= ExprFlags::kHasBinding;
      }
      expr.rhs = ParseBlock();
      expr.span = SpanFrom(start);
      return ast_->Add(std::move(expr));
    }

    default: {
      expr.kind = keyword == TokenKind::kKwFor ? ExprKind::kFor
                                               : ExprKind::kForeach;
      expr.pattern = ParsePattern();
      ExpectToken(TokenKind::kKwIn);
      {
        ScopedFlag allow(&allow_struct_literal_, false);
        expr.lhs = ParseExpression();
      }
      expr.rhs = ParseBlock();
      expr.span = SpanFrom(start);
      return ast_->Add(std::move(expr));
    }
  }
}

ExprId Parser::ParseClosure() {
  const size_t start = pos_;
  std::vector<ParamId> params;

  if (Eat(TokenKind::kPipePipe)) {
    // An empty parameter list.
  } else {
    Advance();  // `|`
    while (!AtEnd() && !At(TokenKind::kPipe)) {
      const size_t param_start = pos_;
      Param param;
      param.is_mutable = Eat(TokenKind::kKwMut);
      param.name = ExpectIdentifier();
      if (!param.name.is_valid()) {
        break;
      }
      if (Eat(TokenKind::kColon)) {
        param.type = ParseType();
      }
      param.span = SpanFrom(param_start);
      params.push_back(ast_->Add(param));
      if (!Eat(TokenKind::kComma)) {
        break;
      }
    }
    ExpectToken(TokenKind::kPipe);
  }

  Expr expr;
  expr.kind = ExprKind::kClosure;
  expr.params = ast_->AddRange(std::span<const ParamId>(params));
  if (Eat(TokenKind::kArrow)) {
    expr.type = ParseType();
  }
  expr.lhs = At(TokenKind::kLeftBrace) ? ParseBlock() : ParseExpression();
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

ExprId Parser::ParseStringLiteral() {
  const size_t start = pos_;
  const Token& token = Advance();
  const StringLiteral& literal = tokens_.StringOf(token);

  std::vector<ExprId> parts;
  for (const StringPart& part : literal.parts) {
    if (part.kind == StringPartKind::kText) {
      Expr text;
      text.kind = ExprKind::kStringText;
      text.span = part.span;
      text.aux = ast_->AddString(part.text);
      parts.push_back(ast_->Add(std::move(text)));
    } else {
      parts.push_back(ParseInterpolation(part.span));
    }
  }

  Expr expr;
  expr.kind = ExprKind::kStringLiteral;
  expr.args = ast_->AddRange(std::span<const ExprId>(parts));
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

ExprId Parser::ParseInterpolation(SourceSpan span) {
  if (span.length() == 0) {
    Expr expr;
    expr.kind = ExprKind::kError;
    expr.flags |= ExprFlags::kHasError;
    expr.span = span;
    return ast_->Add(std::move(expr));
  }

  // The expression lives inside a string token, so it is lexed on demand. The
  // sub-lexer keeps offsets absolute, which keeps diagnostics pointing at the
  // right place inside the literal.
  const TokenStream nested =
      LexSpan(sources_, span, interner_, diagnostics_, LexOptions{});
  Parser parser(sources_, nested, interner_, diagnostics_, ast_, options_);
  const ExprId expr = parser.ParseWholeExpression();
  errors_ += parser.errors_;
  return expr;
}

ExprId Parser::ParseParenthesized() {
  const size_t start = pos_;
  Advance();  // `(`
  ScopedFlag allow(&allow_struct_literal_, true);

  std::vector<ExprId> elements;
  bool is_tuple = false;
  while (!AtEnd() && !At(TokenKind::kRightParen)) {
    const size_t before = pos_;
    elements.push_back(ParseExpression());
    if (Eat(TokenKind::kComma)) {
      is_tuple = true;
    } else {
      break;
    }
    if (pos_ == before) {
      break;
    }
  }
  ExpectToken(TokenKind::kRightParen);

  if (elements.size() == 1 && !is_tuple) {
    return elements.front();
  }

  Expr expr;
  expr.kind = ExprKind::kTupleLiteral;
  expr.args = ast_->AddRange(std::span<const ExprId>(elements));
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

ExprId Parser::ParseArrayLiteral() {
  const size_t start = pos_;
  Advance();  // `[`
  ScopedFlag allow(&allow_struct_literal_, true);

  std::vector<ExprId> elements;
  while (!AtEnd() && !At(TokenKind::kRightBracket)) {
    const size_t before = pos_;
    elements.push_back(ParseExpression());
    if (!Eat(TokenKind::kComma)) {
      break;
    }
    if (pos_ == before) {
      break;
    }
  }
  ExpectToken(TokenKind::kRightBracket);

  Expr expr;
  expr.kind = ExprKind::kArrayLiteral;
  expr.args = ast_->AddRange(std::span<const ExprId>(elements));
  expr.span = SpanFrom(start);
  return ast_->Add(std::move(expr));
}

ExprId Parser::ParsePrimary() {
  const size_t start = pos_;

  switch (PeekKind()) {
    case TokenKind::kIntLiteral:
    case TokenKind::kFloatLiteral: {
      const Token& token = Advance();
      Expr expr;
      expr.kind = token.kind == TokenKind::kIntLiteral
                      ? ExprKind::kIntLiteral
                      : ExprKind::kFloatLiteral;
      expr.aux = ast_->AddNumber(tokens_.NumberOf(token));
      expr.span = SpanOf(token);
      return ast_->Add(std::move(expr));
    }

    case TokenKind::kStringLiteral:
      return ParseStringLiteral();

    case TokenKind::kCharLiteral: {
      const Token& token = Advance();
      Expr expr;
      expr.kind = ExprKind::kCharLiteral;
      expr.aux = tokens_.CharValueOf(token);
      expr.span = SpanOf(token);
      return ast_->Add(std::move(expr));
    }

    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse: {
      const Token& token = Advance();
      Expr expr;
      expr.kind = ExprKind::kBoolLiteral;
      expr.aux = token.kind == TokenKind::kKwTrue ? 1 : 0;
      expr.span = SpanOf(token);
      return ast_->Add(std::move(expr));
    }

    case TokenKind::kKwNull: {
      const Token& token = Advance();
      Expr expr;
      expr.kind = ExprKind::kNullLiteral;
      expr.span = SpanOf(token);
      return ast_->Add(std::move(expr));
    }

    case TokenKind::kKwSelf: {
      const Token& token = Advance();
      Expr expr;
      expr.kind = ExprKind::kSelf;
      expr.span = SpanOf(token);
      return ast_->Add(std::move(expr));
    }

    case TokenKind::kLeftParen:
      return ParseParenthesized();
    case TokenKind::kLeftBracket:
      return ParseArrayLiteral();
    case TokenKind::kLeftBrace:
      return ParseBlock();
    case TokenKind::kKwIf:
      return ParseIf();
    case TokenKind::kKwMatch:
      return ParseMatch();
    case TokenKind::kKwLoop:
    case TokenKind::kKwWhile:
    case TokenKind::kKwFor:
    case TokenKind::kKwForeach:
      return ParseLoopLike(PeekKind());

    case TokenKind::kIdentifier: {
      // `outer: loop { ... }` labels the loop so `break outer` can name it.
      if (PeekKind(1) != TokenKind::kColon || !StartsLoop(PeekKind(2))) {
        break;
      }
      const Symbol label = tokens_.IdentifierOf(Advance());
      Advance();  // `:`
      labels_.push_back(label);
      const ExprId loop = ParseLoopLike(PeekKind(), label);
      labels_.pop_back();
      return loop;
    }
    case TokenKind::kPipe:
    case TokenKind::kPipePipe:
      return ParseClosure();

    case TokenKind::kKwUnsafe:
    case TokenKind::kKwArena: {
      const bool is_unsafe = At(TokenKind::kKwUnsafe);
      Advance();
      Expr expr;
      expr.kind = is_unsafe ? ExprKind::kUnsafe : ExprKind::kRegion;
      expr.lhs = ParseBlock();
      expr.span = SpanFrom(start);
      return ast_->Add(std::move(expr));
    }

    default:
      break;
  }

  if (At(TokenKind::kIdentifier)) {
    const Token& token = Advance();
    Expr expr;
    expr.kind = ExprKind::kPath;
    expr.name = tokens_.IdentifierOf(token);
    expr.span = SpanOf(token);
    return ast_->Add(std::move(expr));
  }

  std::string message = "expected an expression, found ";
  message.append(DescribeCurrent());
  Report(kExpectedExpression, std::move(message), CurrentSpan());
  return MakeError(start);
}

// ------------------------------------------------------------- patterns -----

bool Parser::EatPatternQuestion() {
  if (pending_question_) {
    pending_question_ = false;
    return true;
  }
  if (Eat(TokenKind::kQuestion)) {
    return true;
  }
  if (At(TokenKind::kQuestionQuestion)) {
    // `inner??` is one token that unwraps two levels of optional.
    Advance();
    pending_question_ = true;
    return true;
  }
  return false;
}

bool Parser::AtPatternStart() const {
  switch (PeekKind()) {
    case TokenKind::kUnderscore:
    case TokenKind::kDotDot:
    case TokenKind::kIntLiteral:
    case TokenKind::kFloatLiteral:
    case TokenKind::kStringLiteral:
    case TokenKind::kCharLiteral:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse:
    case TokenKind::kKwNull:
    case TokenKind::kMinus:
    case TokenKind::kLeftParen:
    case TokenKind::kKwMut:
    case TokenKind::kIdentifier:
      return true;
    default:
      return false;
  }
}

AstRange<PatternId> Parser::ParsePatternAlternatives() {
  std::vector<PatternId> patterns;
  if (!AtPatternStart()) {
    return ast_->AddRange(std::span<const PatternId>(patterns));
  }
  for (;;) {
    const PatternId pattern = ParsePattern();
    if (!pattern.is_valid()) {
      break;
    }
    patterns.push_back(pattern);
    if (!Eat(TokenKind::kPipe)) {
      break;
    }
  }
  return ast_->AddRange(std::span<const PatternId>(patterns));
}

PatternId Parser::ParsePattern() {
  const size_t start = pos_;
  Pattern pattern;

  switch (PeekKind()) {
    case TokenKind::kUnderscore: {
      Advance();
      pattern.kind = PatternKind::kWildcard;
      pattern.span = SpanFrom(start);
      PatternId current = ast_->Add(pattern);
      // `_?` matches any present optional without binding it.
      while (EatPatternQuestion()) {
        Pattern present;
        present.kind = PatternKind::kPresent;
        std::array<PatternId, 1> elements = {current};
        present.elements =
            ast_->AddRange(std::span<const PatternId>(elements));
        present.span = SpanFrom(start);
        current = ast_->Add(present);
      }
      return current;
    }

    case TokenKind::kDotDot:
      Advance();
      pattern.kind = PatternKind::kRest;
      pattern.span = SpanFrom(start);
      return ast_->Add(pattern);

    case TokenKind::kIntLiteral:
    case TokenKind::kFloatLiteral:
    case TokenKind::kStringLiteral:
    case TokenKind::kCharLiteral:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse:
    case TokenKind::kKwNull:
    case TokenKind::kMinus:
      pattern.kind = PatternKind::kLiteral;
      pattern.literal = ParseUnary();
      pattern.span = SpanFrom(start);
      return ast_->Add(pattern);

    case TokenKind::kLeftParen: {
      Advance();
      pattern.kind = PatternKind::kTuple;
      std::vector<PatternId> elements;
      while (!AtEnd() && !At(TokenKind::kRightParen)) {
        const size_t before = pos_;
        elements.push_back(ParsePattern());
        if (!Eat(TokenKind::kComma)) {
          break;
        }
        if (pos_ == before) {
          break;
        }
      }
      ExpectToken(TokenKind::kRightParen);
      pattern.elements = ast_->AddRange(std::span<const PatternId>(elements));
      pattern.span = SpanFrom(start);
      return ast_->Add(pattern);
    }

    case TokenKind::kKwMut: {
      Advance();
      pattern.kind = PatternKind::kBinding;
      pattern.flags |= PatternFlags::kMutable;
      pattern.name = ExpectIdentifier();
      pattern.span = SpanFrom(start);
      return ast_->Add(pattern);
    }

    case TokenKind::kIdentifier:
      break;

    default: {
      std::string message = "expected a pattern, found ";
      message.append(DescribeCurrent());
      Report(kExpectedPattern, std::move(message), CurrentSpan());
      pattern.kind = PatternKind::kError;
      pattern.flags |= PatternFlags::kHasError;
      pattern.span = SpanFrom(start);
      return ast_->Add(pattern);
    }
  }

  const AstRange<Symbol> path = ParseDottedPath();
  const std::span<const Symbol> segments = ast_->Get(path);

  // `n?` matches a present optional and binds its contents, which is the
  // counterpart of the `null` literal pattern. No Some/None vocabulary is
  // needed: the type spells it `T?` and the pattern spells it `n?`.
  if ((At(TokenKind::kQuestion) || At(TokenKind::kQuestionQuestion)) &&
      segments.size() == 1) {
    Pattern inner;
    inner.kind = PatternKind::kBinding;
    inner.name = segments.front();
    inner.span = SpanFrom(start);
    PatternId current = ast_->Add(inner);

    while (EatPatternQuestion()) {
      Pattern present;
      present.kind = PatternKind::kPresent;
      std::array<PatternId, 1> elements = {current};
      present.elements =
          ast_->AddRange(std::span<const PatternId>(elements));
      present.span = SpanFrom(start);
      current = ast_->Add(present);
    }
    return current;
  }

  if (At(TokenKind::kLeftParen)) {
    Advance();
    pattern.kind = PatternKind::kVariant;
    pattern.path = path;
    std::vector<PatternId> elements;
    while (!AtEnd() && !At(TokenKind::kRightParen)) {
      const size_t before = pos_;
      elements.push_back(ParsePattern());
      if (!Eat(TokenKind::kComma)) {
        break;
      }
      if (pos_ == before) {
        break;
      }
    }
    ExpectToken(TokenKind::kRightParen);
    pattern.elements = ast_->AddRange(std::span<const PatternId>(elements));
    pattern.span = SpanFrom(start);
    return ast_->Add(pattern);
  }

  if (At(TokenKind::kLeftBrace)) {
    const SourceSpan open = CurrentSpan();
    Advance();
    pattern.kind = PatternKind::kStruct;
    pattern.path = path;

    std::vector<PatternId> fields;
    while (!AtEnd() && !At(TokenKind::kRightBrace)) {
      const size_t field_start = pos_;
      const Symbol name = ExpectIdentifier();
      if (!name.is_valid()) {
        break;
      }
      PatternId value;
      if (Eat(TokenKind::kColon)) {
        value = ParsePattern();
      } else {
        Pattern binding;
        binding.kind = PatternKind::kBinding;
        binding.name = name;
        binding.span = SpanFrom(field_start);
        value = ast_->Add(binding);
      }
      if (value.is_valid()) {
        ast_->GetMutable(value).field = name;
        fields.push_back(value);
      }
      if (!Eat(TokenKind::kComma)) {
        break;
      }
    }
    ExpectCloseBrace(open);
    pattern.elements = ast_->AddRange(std::span<const PatternId>(fields));
    pattern.span = SpanFrom(start);
    return ast_->Add(pattern);
  }

  if (segments.size() == 1) {
    pattern.kind = PatternKind::kBinding;
    pattern.name = segments.front();
  } else {
    pattern.kind = PatternKind::kVariant;
    pattern.path = path;
  }
  pattern.span = SpanFrom(start);
  return ast_->Add(pattern);
}

// ---------------------------------------------------------------- types -----

AstRange<Symbol> Parser::ParseDottedPath() {
  std::vector<Symbol> segments;
  for (;;) {
    const Symbol segment = ExpectIdentifier();
    if (!segment.is_valid()) {
      break;
    }
    segments.push_back(segment);
    if (!At(TokenKind::kDot) || PeekKind(1) != TokenKind::kIdentifier) {
      break;
    }
    Advance();
  }
  return ast_->AddRange(std::span<const Symbol>(segments));
}

bool Parser::AtAngleClose() const {
  return pending_angle_close_ || At(TokenKind::kGreater) ||
         At(TokenKind::kShr);
}

bool Parser::EatAngleClose() {
  if (pending_angle_close_) {
    pending_angle_close_ = false;
    return true;
  }
  if (Eat(TokenKind::kGreater)) {
    return true;
  }
  if (At(TokenKind::kShr)) {
    // `Map<K, Array<V>>` ends with one token that closes two lists.
    Advance();
    pending_angle_close_ = true;
    return true;
  }
  return false;
}

AstRange<TypeRefId> Parser::ParseGenericArguments() {
  std::vector<TypeRefId> args;
  if (!Eat(TokenKind::kLess)) {
    return ast_->AddRange(std::span<const TypeRefId>(args));
  }

  while (!AtEnd() && !AtAngleClose()) {
    const size_t before = pos_;
    args.push_back(ParseType());
    if (!Eat(TokenKind::kComma)) {
      break;
    }
    if (pos_ == before) {
      break;
    }
  }

  if (!EatAngleClose()) {
    std::string message = "expected `>` to close the generic arguments, ";
    message.append("found ");
    message.append(DescribeCurrent());
    Report(kExpectedToken, std::move(message), CurrentSpan());
  }
  return ast_->AddRange(std::span<const TypeRefId>(args));
}

TypeRefId Parser::ParseTypeNoSuffix() {
  const size_t start = pos_;
  TypeRef type;

  switch (PeekKind()) {
    case TokenKind::kKwMut: {
      Advance();
      type.kind = TypeRefKind::kMut;
      type.flags |= TypeRefFlags::kMutable;
      type.element = ParseType();
      type.span = SpanFrom(start);
      return ast_->Add(type);
    }

    case TokenKind::kStar:
    case TokenKind::kAmp: {
      const bool is_pointer = At(TokenKind::kStar);
      Advance();
      type.kind = is_pointer ? TypeRefKind::kPointer : TypeRefKind::kReference;
      if (Eat(TokenKind::kKwMut)) {
        type.flags |= TypeRefFlags::kMutable;
      }
      type.element = ParseType();
      type.span = SpanFrom(start);
      return ast_->Add(type);
    }

    case TokenKind::kKwFun: {
      Advance();
      type.kind = TypeRefKind::kFunction;
      std::vector<TypeRefId> params;
      if (ExpectToken(TokenKind::kLeftParen)) {
        while (!AtEnd() && !At(TokenKind::kRightParen)) {
          const size_t before = pos_;
          params.push_back(ParseType());
          if (!Eat(TokenKind::kComma)) {
            break;
          }
          if (pos_ == before) {
            break;
          }
        }
        ExpectToken(TokenKind::kRightParen);
      }
      type.args = ast_->AddRange(std::span<const TypeRefId>(params));
      if (Eat(TokenKind::kArrow)) {
        type.element = ParseType();
      }
      type.span = SpanFrom(start);
      return ast_->Add(type);
    }

    case TokenKind::kLeftParen: {
      Advance();
      type.kind = TypeRefKind::kTuple;
      std::vector<TypeRefId> elements;
      while (!AtEnd() && !At(TokenKind::kRightParen)) {
        const size_t before = pos_;
        elements.push_back(ParseType());
        if (!Eat(TokenKind::kComma)) {
          break;
        }
        if (pos_ == before) {
          break;
        }
      }
      ExpectToken(TokenKind::kRightParen);
      type.args = ast_->AddRange(std::span<const TypeRefId>(elements));
      type.span = SpanFrom(start);
      return ast_->Add(type);
    }

    // `[]T` is a slice and `[N]T` an array: the brackets say what the shape
    // is, and the element follows. Left to right, and shorter than putting
    // the length after the element.
    case TokenKind::kLeftBracket: {
      Advance();
      if (Eat(TokenKind::kRightBracket)) {
        type.kind = TypeRefKind::kSlice;
      } else {
        type.kind = TypeRefKind::kArray;
        type.length = ParseExpression();
        if (!type.length.is_valid()) {
          Report(kExpectedArraySize, "expected an array length",
                 CurrentSpan());
        }
        ExpectToken(TokenKind::kRightBracket);
      }
      type.element = ParseType();
      type.span = SpanFrom(start);
      return ast_->Add(type);
    }

    case TokenKind::kIdentifier:
      break;

    default: {
      std::string message = "expected a type, found ";
      message.append(DescribeCurrent());
      Report(kExpectedType, std::move(message), CurrentSpan());
      type.kind = TypeRefKind::kError;
      type.flags |= TypeRefFlags::kHasError;
      type.span = SpanFrom(start);
      return ast_->Add(type);
    }
  }

  // `soa [Particle]` and `aos [Particle]` choose the storage layout. The
  // modifier belongs to the storage, so the element type stays usable on its
  // own and the choice can be flipped where it is measured.
  if ((AtContextual(ContextualKeyword::kSoa) ||
       AtContextual(ContextualKeyword::kAos)) &&
      (PeekKind(1) == TokenKind::kLeftBracket ||
       PeekKind(1) == TokenKind::kIdentifier)) {
    const bool is_soa = AtContextual(ContextualKeyword::kSoa);
    Advance();
    TypeRefId storage = ParseType();
    if (ast_->Contains(storage)) {
      TypeRef& stored = ast_->GetMutable(storage);
      stored.flags |= is_soa ? TypeRefFlags::kSoa : TypeRefFlags::kAos;
      stored.span = SpanFrom(start);
    }
    return storage;
  }

  // Query filters are contextual: `with Tag` and `without Tag` are only
  // modifiers when a type follows them.
  if ((AtContextual(ContextualKeyword::kWith) ||
       AtContextual(ContextualKeyword::kWithout)) &&
      PeekKind(1) == TokenKind::kIdentifier) {
    type.kind = AtContextual(ContextualKeyword::kWith) ? TypeRefKind::kWith
                                                       : TypeRefKind::kWithout;
    Advance();
    type.element = ParseType();
    type.span = SpanFrom(start);
    return ast_->Add(type);
  }

  type.kind = TypeRefKind::kPath;
  type.path = ParseDottedPath();
  if (At(TokenKind::kLess)) {
    type.args = ParseGenericArguments();
  }
  type.span = SpanFrom(start);
  return ast_->Add(type);
}

TypeRefId Parser::ParseType() {
  ScopedDepth depth(&depth_);
  if (depth_ > kMaxNesting) {
    const size_t start = pos_;
    Report(kNestingTooDeep, "type nests too deeply", CurrentSpan());
    TypeRef type;
    type.kind = TypeRefKind::kError;
    type.flags |= TypeRefFlags::kHasError;
    type.span = SpanFrom(start);
    return ast_->Add(type);
  }

  const size_t start = pos_;
  TypeRefId inner = ParseTypeNoSuffix();

  // `T?` and `T!` are both postfix, so a type reads left to right: `Pawn?!`
  // is "a Pawn, possibly absent, fallibly".
  for (;;) {
    if (At(TokenKind::kQuestion)) {
      Advance();
      TypeRef optional;
      optional.kind = TypeRefKind::kOptional;
      optional.element = inner;
      optional.span = SpanFrom(start);
      inner = ast_->Add(optional);
      continue;
    }
    if (At(TokenKind::kBang)) {
      Advance();
      TypeRef fallible;
      fallible.kind = TypeRefKind::kFallible;
      fallible.element = inner;
      fallible.span = SpanFrom(start);
      inner = ast_->Add(fallible);
      continue;
    }
    break;
  }
  return inner;
}

}  // namespace

Ast Parse(const SourceManager& sources,
          const TokenStream& tokens,
          StringInterner* interner,
          DiagnosticEngine* diagnostics,
          const ParseOptions& options) {
  WOLANG_CHECK(interner != nullptr);
  WOLANG_CHECK(diagnostics != nullptr);

  Ast ast(tokens.file);
  Parser parser(sources, tokens, interner, diagnostics, &ast, options);
  parser.ParseFile();
  return ast;
}

}  // namespace wolang
