// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/format/formatter.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace wolang {
namespace {

// Postfix forms bind tighter than any prefix operator, so an operand that is
// itself a prefix expression needs parentheses in that position.
constexpr Precedence kPostfixContext =
    static_cast<Precedence>(static_cast<int>(Precedence::kUnary) + 1);

Precedence Tighter(Precedence precedence) {
  return static_cast<Precedence>(static_cast<int>(precedence) + 1);
}

// Renders a float so that re-reading the result yields the same value and the
// same token kind. The shortest precision that round-trips is used, and a
// `.0` is appended when the digits alone would lex as an integer.
//
// `%g` switches to an exponent as soon as that saves a character, which turns
// `40.0` into `4e+01`: correct, round-tripping, and not what anybody writes.
// The source notation is not in the tree, so the formatter picks one by
// magnitude -- plain within the range ordinary code lives in, an exponent
// outside it. The cost is that `1e10` comes back expanded; the alternative
// cost was every `40.0` in a gameplay file turning into `4e+01`.
std::string FormatFloatLiteral(double value) {
  char buffer[340];
  const auto shortest = [&buffer](const char* format, double value) {
    for (int precision = 1; precision <= 17; ++precision) {
      std::snprintf(buffer, sizeof(buffer), format, precision, value);
      if (std::strtod(buffer, nullptr) == value) {
        return true;
      }
    }
    return false;
  };

  const double magnitude = std::fabs(value);
  const bool plain_range =
      magnitude == 0.0 || (magnitude >= 1e-4 && magnitude < 1e16);

  std::string text;
  if (plain_range && shortest("%.*f", value)) {
    text = buffer;
    // `%f` pads with zeros the round-trip did not need.
    if (text.find('.') != std::string::npos) {
      while (text.size() > 1 && text.back() == '0' &&
             text[text.size() - 2] != '.') {
        text.pop_back();
      }
    }
  } else {
    shortest("%.*g", value);
    text = buffer;
  }

  if (text.find_first_of(".eEni") == std::string::npos) {
    text.append(".0");
  }
  return text;
}

std::string FormatUint64(uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu",
                static_cast<unsigned long long>(value));
  return std::string(buffer);
}

std::string_view TrimLeft(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  return text.substr(begin);
}

std::string_view TrimRight(std::string_view text) {
  size_t end = text.size();
  while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                     text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(0, end);
}

class Formatter {
 public:
  Formatter(const SourceManager& sources,
            const StringInterner& interner,
            const TokenStream& tokens,
            const Ast& ast,
            const FormatOptions& options)
      : sources_(sources),
        interner_(interner),
        tokens_(tokens),
        ast_(ast),
        options_(options),
        text_(sources.Text(tokens.file)) {}

  std::string Run() {
    // The lexer skips a shebang as trivia, so it is not in the tree. It has
    // to be written back, or `fmt --write` would silently make an executable
    // script stop being one.
    if (text_.size() >= 2 && text_[0] == '#' && text_[1] == '!') {
      const size_t end = text_.find('\n');
      out_.append(TrimRight(
          text_.substr(0, end == std::string_view::npos ? text_.size() : end)));
      out_.push_back('\n');
    }

    for (ItemId id : ast_.Get(ast_.root())) {
      const Item& item = ast_.Get(id);
      FlushCommentsBefore(item.span.begin, !out_.empty());
      if (!out_.empty() && HasBlankLineBefore(item.span.begin)) {
        NewLine();
      }
      EmitItem(id);
      NewLine();
    }

    // Anything left at the end of the file is a trailing comment block.
    FlushCommentsBefore(static_cast<uint32_t>(text_.size()), true);
    return std::move(out_);
  }

 private:
  // ---------------------------------------------------------------- output --

  void Put(std::string_view piece) {
    if (pending_indent_) {
      out_.append(static_cast<size_t>(indent_), ' ');
      pending_indent_ = false;
    }
    out_.append(piece);
  }

  void NewLine() {
    if (pending_indent_) {
      // An empty line carries no indentation.
      pending_indent_ = false;
    }
    out_.push_back('\n');
    pending_indent_ = true;
  }

  uint32_t Column() const {
    if (pending_indent_) {
      return static_cast<uint32_t>(indent_);
    }
    const size_t last = out_.rfind('\n');
    const size_t begin = last == std::string::npos ? 0 : last + 1;
    return static_cast<uint32_t>(out_.size() - begin);
  }

  void PushIndent() { indent_ += static_cast<int>(options_.indent_width); }
  void PopIndent() { indent_ -= static_cast<int>(options_.indent_width); }

  bool Fits(const std::string& piece) const {
    return piece.find('\n') == std::string::npos &&
           Column() + piece.size() <= options_.column_limit;
  }

  // Renders |body| into a scratch buffer so the caller can decide whether the
  // one-line form fits. Comments are never emitted while capturing, so a
  // construct that contains one is always written out in its broken form.
  template <typename Body>
  std::string Capture(Body body) {
    std::string saved = std::move(out_);
    const bool saved_pending = pending_indent_;
    const uint32_t saved_comment = comment_index_;
    out_.clear();
    pending_indent_ = false;
    ++capturing_;

    body();

    --capturing_;
    std::string captured = std::move(out_);
    out_ = std::move(saved);
    pending_indent_ = saved_pending;
    comment_index_ = saved_comment;
    return captured;
  }

  // -------------------------------------------------------------- comments --

  bool HasCommentsIn(uint32_t begin, uint32_t end) const {
    for (size_t i = comment_index_; i < tokens_.comments.size(); ++i) {
      const SourceSpan& comment = tokens_.comments[i];
      if (comment.begin >= end) {
        return false;
      }
      if (comment.begin >= begin) {
        return true;
      }
    }
    return false;
  }

  std::string_view CommentText(const SourceSpan& comment) const {
    return TrimRight(sources_.SpanText(comment));
  }

  // Emits every pending comment that starts before |offset|, each on its own
  // line, preserving a single blank line where the source had one.
  void FlushCommentsBefore(uint32_t offset, bool allow_blank_line) {
    while (comment_index_ < tokens_.comments.size() &&
           tokens_.comments[comment_index_].begin < offset) {
      const SourceSpan comment = tokens_.comments[comment_index_];
      ++comment_index_;
      if (capturing_ > 0) {
        continue;
      }
      if (allow_blank_line && HasBlankLineBefore(comment.begin)) {
        NewLine();
      }
      Put(CommentText(comment));
      NewLine();
      allow_blank_line = true;
    }
  }

  // Emits a comment that sat at the end of the line just written.
  void PutTrailingComment(uint32_t end_offset) {
    if (capturing_ > 0 || comment_index_ >= tokens_.comments.size()) {
      return;
    }
    const SourceSpan comment = tokens_.comments[comment_index_];
    if (comment.begin < end_offset) {
      return;
    }
    const uint32_t construct_line =
        sources_.Position(tokens_.file, end_offset).line;
    const uint32_t comment_line =
        sources_.Position(comment.file, comment.begin).line;
    if (construct_line != comment_line) {
      return;
    }
    ++comment_index_;
    Put("  ");
    Put(CommentText(comment));
  }

  // True when the source had an empty line immediately before |offset|.
  bool HasBlankLineBefore(uint32_t offset) const {
    if (offset == 0 || offset > text_.size()) {
      return false;
    }
    size_t i = offset;
    int newlines = 0;
    while (i > 0) {
      const char c = text_[i - 1];
      if (c == '\n') {
        ++newlines;
        if (newlines == 2) {
          return true;
        }
      } else if (c != ' ' && c != '\t' && c != '\r') {
        return false;
      }
      --i;
    }
    return false;
  }

  void EmitDoc(SourceSpan doc) {
    if (!doc.file.is_valid() || doc.length() == 0) {
      return;
    }
    const std::string_view text = sources_.SpanText(doc);
    size_t position = 0;
    while (position <= text.size()) {
      const size_t newline = text.find('\n', position);
      const size_t length =
          (newline == std::string_view::npos ? text.size() : newline) -
          position;
      Put(TrimRight(TrimLeft(text.substr(position, length))));
      NewLine();
      if (newline == std::string_view::npos) {
        break;
      }
      position = newline + 1;
    }
  }

  // ----------------------------------------------------------------- names --

  std::string_view Name(Symbol symbol) const {
    return symbol.is_valid() ? interner_.Text(symbol) : "_";
  }

  void PutPath(AstRange<Symbol> path) {
    bool first = true;
    for (Symbol segment : ast_.Get(path)) {
      if (!first) {
        Put(".");
      }
      first = false;
      Put(Name(segment));
    }
  }

  // ----------------------------------------------------------------- items --

  void EmitModifiers(const Item& item) {
    if (HasFlag(item.flags, ItemFlags::kOffer)) {
      Put("offer ");
    }
    if (HasFlag(item.flags, ItemFlags::kPriv)) {
      Put("priv ");
    }
    if (HasFlag(item.flags, ItemFlags::kComptime)) {
      Put("comptime ");
    }
    if (HasFlag(item.flags, ItemFlags::kExtern)) {
      Put("extern ");
      if (item.abi.is_valid()) {
        Put("\"");
        Put(Name(item.abi));
        Put("\" ");
      }
    }
    if (HasFlag(item.flags, ItemFlags::kAsync)) {
      Put("task ");
    }
    if (HasFlag(item.flags, ItemFlags::kVirtual)) {
      Put("virtual ");
    }
    if (HasFlag(item.flags, ItemFlags::kOverride)) {
      Put("override ");
    }
    if (HasFlag(item.flags, ItemFlags::kDerived)) {
      Put("derived ");
    }
    if (HasFlag(item.flags, ItemFlags::kWeak)) {
      Put("weak ");
    }
  }

  void EmitAttributes(AstRange<AttributeId> attributes) {
    if (attributes.empty()) {
      return;
    }
    // Attributes live on their own line above the declaration, together when
    // they fit and one per line when they do not.
    const std::string inline_form = Capture([&] {
      bool first = true;
      for (AttributeId id : ast_.Get(attributes)) {
        if (!first) {
          Put(" ");
        }
        first = false;
        EmitAttribute(id);
      }
    });

    if (Fits(inline_form)) {
      Put(inline_form);
      NewLine();
      return;
    }
    for (AttributeId id : ast_.Get(attributes)) {
      EmitAttribute(id);
      NewLine();
    }
  }

  void EmitAttribute(AttributeId id) {
    const Attribute& attribute = ast_.Get(id);
    Put("@");
    Put(Name(attribute.name));
    if (attribute.args.empty()) {
      return;
    }
    Put("(");
    bool first = true;
    for (ExprId arg : ast_.Get(attribute.args)) {
      if (!first) {
        Put(", ");
      }
      first = false;
      EmitExpr(arg);
    }
    Put(")");
  }

  void EmitGenerics(AstRange<GenericParamId> generics) {
    if (generics.empty()) {
      return;
    }
    Put("<");
    bool first = true;
    for (GenericParamId id : ast_.Get(generics)) {
      if (!first) {
        Put(", ");
      }
      first = false;
      const GenericParam& param = ast_.Get(id);
      Put(Name(param.name));
      bool first_bound = true;
      for (TypeRefId bound : ast_.Get(param.bounds)) {
        Put(first_bound ? ": " : " + ");
        first_bound = false;
        EmitType(bound);
      }
    }
    Put(">");
  }

  void EmitParams(AstRange<ParamId> params) {
    const std::string inline_form = Capture([&] {
      Put("(");
      bool first = true;
      for (ParamId id : ast_.Get(params)) {
        if (!first) {
          Put(", ");
        }
        first = false;
        EmitParam(id);
      }
      Put(")");
    });

    if (Fits(inline_form)) {
      Put(inline_form);
      return;
    }

    // One item per line, double indented, with the closing delimiter attached
    // to the last one so the construct ends where its content ends.
    Put("(");
    NewLine();
    PushIndent();
    PushIndent();
    const std::span<const ParamId> ids = ast_.Get(params);
    for (size_t i = 0; i < ids.size(); ++i) {
      EmitParam(ids[i]);
      if (i + 1 < ids.size()) {
        Put(",");
        NewLine();
      }
    }
    Put(")");
    PopIndent();
    PopIndent();
  }

  void EmitParam(ParamId id) {
    const Param& param = ast_.Get(id);
    for (AttributeId attribute : ast_.Get(param.attributes)) {
      EmitAttribute(attribute);
      Put(" ");
    }
    if (param.is_mutable) {
      Put("mut ");
    }
    Put(Name(param.name));
    if (param.type.is_valid()) {
      Put(": ");
      EmitType(param.type);
    }
    if (param.default_value.is_valid()) {
      Put(" = ");
      EmitExpr(param.default_value);
    }
  }

  void EmitEffects(AstRange<TypeRefId> effects) {
    if (effects.empty()) {
      return;
    }
    Put(" uses ");
    bool first = true;
    for (TypeRefId effect : ast_.Get(effects)) {
      if (!first) {
        Put(", ");
      }
      first = false;
      EmitType(effect);
    }
  }

  void EmitMemberBlock(AstRange<ItemId> members, uint32_t end_offset) {
    if (members.empty() && !HasCommentsIn(0, end_offset)) {
      Put(" {}");
      return;
    }
    Put(" {");
    NewLine();
    PushIndent();

    bool first = true;
    for (ItemId id : ast_.Get(members)) {
      const Item& member = ast_.Get(id);
      FlushCommentsBefore(member.span.begin, !first);
      if (!first && HasBlankLineBefore(member.span.begin)) {
        NewLine();
      }
      first = false;
      EmitItem(id);
      NewLine();
    }
    FlushCommentsBefore(end_offset, true);

    PopIndent();
    Put("}");
  }

  void EmitItem(ItemId id) {
    const Item& item = ast_.Get(id);
    EmitDoc(item.doc);
    EmitAttributes(item.attributes);
    EmitModifiers(item);

    switch (item.kind) {
      case ItemKind::kNeed: {
        Put("need ");
        PutPath(item.path);
        if (HasFlag(item.flags, ItemFlags::kNeedList)) {
          Put(".{");
          bool first = true;
          for (Symbol name : ast_.Get(item.names)) {
            if (!first) {
              Put(", ");
            }
            first = false;
            Put(Name(name));
          }
          Put("}");
        }
        break;
      }

      case ItemKind::kFunction: {
        Put("fun ");
        Put(Name(item.name));
        EmitGenerics(item.generics);
        EmitParams(item.params);
        if (item.type.is_valid()) {
          Put(" -> ");
          EmitType(item.type);
        }
        EmitEffects(item.effects);
        if (item.link_name.is_valid()) {
          Put(" = \"");
          Put(Name(item.link_name));
          Put("\"");
        } else if (HasFlag(item.flags, ItemFlags::kExpressionBody)) {
          Put(" => ");
          EmitExpr(item.body);
        } else if (item.body.is_valid()) {
          EmitBlockBody(item.body);
        }
        break;
      }

      case ItemKind::kClass:
      case ItemKind::kStruct:
      case ItemKind::kAbility:
      case ItemKind::kComponent: {
        Put(ItemKindName(item.kind));
        Put(" ");
        Put(Name(item.name));
        EmitGenerics(item.generics);
        if (item.super_type.is_valid()) {
          Put(" : ");
          EmitType(item.super_type);
        }
        EmitMemberBlock(item.members, item.span.end);
        break;
      }

      case ItemKind::kEnum: {
        Put("enum ");
        Put(Name(item.name));
        EmitGenerics(item.generics);
        EmitMemberBlock(item.members, item.span.end);
        break;
      }

      case ItemKind::kVariant: {
        Put(Name(item.name));
        if (!item.params.empty()) {
          EmitParams(item.params);
        }
        break;
      }

      case ItemKind::kExtend: {
        Put("extend");
        EmitGenerics(item.generics);
        Put(" ");
        EmitType(item.target_type);
        if (HasFlag(item.flags, ItemFlags::kAbilityImpl)) {
          Put(" as ");
          EmitType(item.super_type);
        }
        EmitMemberBlock(item.members, item.span.end);
        break;
      }

      case ItemKind::kErrorSet: {
        Put("error ");
        bool first = true;
        for (Symbol name : ast_.Get(item.names)) {
          if (!first) {
            Put(", ");
          }
          first = false;
          Put(Name(name));

          bool first_field = true;
          for (ParamId field : ast_.Get(item.params)) {
            if (ast_.Get(field).error != name) {
              continue;
            }
            Put(first_field ? "(" : ", ");
            first_field = false;
            EmitParam(field);
          }
          if (!first_field) {
            Put(")");
          }
        }
        break;
      }

      case ItemKind::kTag:
        Put("tag ");
        Put(Name(item.name));
        break;

      case ItemKind::kSystem: {
        Put("system ");
        Put(Name(item.name));
        EmitParams(item.params);
        EmitEffects(item.effects);
        for (OrderingId ordering_id : ast_.Get(item.orderings)) {
          const Ordering& ordering = ast_.Get(ordering_id);
          Put(ordering.is_after ? " after " : " before ");
          PutPath(ordering.path);
        }
        if (item.body.is_valid()) {
          EmitBlockBody(item.body);
        }
        break;
      }

      case ItemKind::kSignal:
        Put("signal ");
        Put(Name(item.name));
        EmitParams(item.params);
        break;

      case ItemKind::kOn: {
        Put("on ");
        PutPath(item.path);
        EmitParams(item.params);
        if (item.body.is_valid()) {
          EmitBlockBody(item.body);
        }
        break;
      }

      case ItemKind::kBinding: {
        Put(HasFlag(item.flags, ItemFlags::kMutable) ? "var " : "const ");
        Put(Name(item.name));
        if (item.type.is_valid()) {
          Put(": ");
          EmitType(item.type);
        }
        if (item.init.is_valid()) {
          Put(HasFlag(item.flags, ItemFlags::kExpressionBody) ? " => "
                                                              : " = ");
          EmitExpr(item.init);
        }
        break;
      }

      case ItemKind::kTypeAlias:
        Put("type ");
        Put(Name(item.name));
        EmitGenerics(item.generics);
        if (item.super_type.is_valid()) {
          Put(": ");
          EmitType(item.super_type);
        }
        if (item.type.is_valid()) {
          Put(" = ");
          EmitType(item.type);
        }
        break;

      case ItemKind::kDimension: {
        Put("dimension ");
        bool first = true;
        for (Symbol name : ast_.Get(item.names)) {
          if (!first) {
            Put(", ");
          }
          first = false;
          Put(Name(name));
        }
        // `dimension Velocity = Length / Time`. Dropping the product would
        // silently turn a derived dimension into a base one.
        if (item.init.is_valid()) {
          Put(" = ");
          in_dimension_ = true;
          EmitExpr(item.init);
          in_dimension_ = false;
        }
        break;
      }

      case ItemKind::kUnit:
        Put("unit ");
        Put(Name(item.name));
        if (!item.path.empty()) {
          Put(": ");
          PutPath(item.path);
        } else if (item.init.is_valid()) {
          Put(" = ");
          EmitExpr(item.init);
        }
        break;

      case ItemKind::kEffect:
        Put("effect ");
        Put(Name(item.name));
        break;

      case ItemKind::kMachine: {
        Put("machine ");
        Put(Name(item.name));
        Put(" {");
        NewLine();
        PushIndent();
        bool first = true;
        for (StateId state_id : ast_.Get(item.states)) {
          const State& state = ast_.Get(state_id);
          if (!first && HasBlankLineBefore(state.span.begin)) {
            NewLine();
          }
          first = false;
          EmitState(state_id);
          NewLine();
        }
        PopIndent();
        Put("}");
        break;
      }

      case ItemKind::kDeinit:
        Put("deinit ");
        EmitBlockBody(item.body);
        break;

      case ItemKind::kError:
        Put("// <unparsable declaration>");
        break;
    }

    PutTrailingComment(item.span.end);
  }

  void EmitState(StateId id) {
    const State& state = ast_.Get(id);
    Put("state ");
    Put(Name(state.name));
    Put(" {");
    NewLine();
    PushIndent();

    if (state.enter.is_valid()) {
      Put("enter");
      EmitBlockBody(state.enter);
      NewLine();
    }
    if (state.exit.is_valid()) {
      Put("exit");
      EmitBlockBody(state.exit);
      NewLine();
    }
    for (TransitionId transition_id : ast_.Get(state.transitions)) {
      const Transition& transition = ast_.Get(transition_id);
      if (transition.is_always) {
        Put("always");
      } else {
        Put("on ");
        Put(Name(transition.event));
      }
      if (transition.delay.is_valid()) {
        Put(" after ");
        EmitExpr(transition.delay);
      }
      Put(" -> ");
      Put(Name(transition.target));
      NewLine();
    }

    PopIndent();
    Put("}");
  }

  // ------------------------------------------------------------ statements --

  // Emits a block as the body of a declaration or a control structure, i.e.
  // with a leading space and the brace on the same line.
  void EmitBlockBody(ExprId id) {
    if (!ast_.Contains(id) || ast_.Get(id).kind != ExprKind::kBlock) {
      Put(" ");
      EmitExpr(id);
      return;
    }
    const Expr& block = ast_.Get(id);
    if (block.stmts.empty() &&
        !HasCommentsIn(block.span.begin, block.span.end)) {
      Put(" {}");
      return;
    }

    Put(" {");
    NewLine();
    PushIndent();
    EmitStatements(block);
    PopIndent();
    Put("}");
  }

  void EmitStatements(const Expr& block) {
    bool first = true;
    for (StmtId id : ast_.Get(block.stmts)) {
      const Stmt& stmt = ast_.Get(id);
      FlushCommentsBefore(stmt.span.begin, !first);
      if (!first && HasBlankLineBefore(stmt.span.begin)) {
        NewLine();
      }
      first = false;
      EmitStmt(id);
      NewLine();
    }
    FlushCommentsBefore(block.span.end, true);
  }

  void EmitStmt(StmtId id) {
    const Stmt& stmt = ast_.Get(id);
    switch (stmt.kind) {
      case StmtKind::kLet:
      case StmtKind::kVar:
        Put(stmt.kind == StmtKind::kLet ? "const " : "var ");
        EmitPattern(stmt.pattern);
        if (stmt.type.is_valid()) {
          Put(": ");
          EmitType(stmt.type);
        }
        if (stmt.expr.is_valid()) {
          Put(" = ");
          EmitExpr(stmt.expr);
        }
        break;

      case StmtKind::kExpr:
        EmitExpr(stmt.expr);
        break;

      case StmtKind::kReturn:
        Put("return");
        if (stmt.expr.is_valid()) {
          Put(" ");
          EmitExpr(stmt.expr);
        }
        break;

      case StmtKind::kBreak:
      case StmtKind::kContinue:
        Put(stmt.kind == StmtKind::kBreak ? "break" : "continue");
        if (stmt.label.is_valid()) {
          Put(" ");
          Put(Name(stmt.label));
        }
        break;

      case StmtKind::kDefer:
        Put("defer");
        if (ast_.Contains(stmt.expr) &&
            ast_.Get(stmt.expr).kind == ExprKind::kBlock) {
          EmitBlockBody(stmt.expr);
        } else {
          Put(" ");
          EmitExpr(stmt.expr);
        }
        break;

      case StmtKind::kItem:
        EmitItem(stmt.item);
        break;

      case StmtKind::kError:
        Put("// <unparsable statement>");
        break;
    }

    PutTrailingComment(stmt.span.end);
  }

  // ----------------------------------------------------------- expressions --

  void EmitExpr(ExprId id, Precedence context = Precedence::kNone) {
    if (!ast_.Contains(id)) {
      Put("/* missing */");
      return;
    }
    const Expr& expr = ast_.Get(id);

    switch (expr.kind) {
      case ExprKind::kIntLiteral: {
        const NumberLiteral& number = ast_.number(expr.aux);
        Put(FormatUint64(number.integer_value));
        PutNumberSuffix(number);
        break;
      }

      case ExprKind::kFloatLiteral: {
        const NumberLiteral& number = ast_.number(expr.aux);
        Put(FormatFloatLiteral(number.float_value));
        PutNumberSuffix(number);
        break;
      }

      case ExprKind::kStringLiteral: {
        Put("\"");
        for (ExprId part : ast_.Get(expr.args)) {
          const Expr& piece = ast_.Get(part);
          if (piece.kind == ExprKind::kStringText) {
            PutEscaped(ast_.string(piece.aux));
          } else {
            Put("{");
            EmitExpr(part);
            Put("}");
          }
        }
        Put("\"");
        break;
      }

      case ExprKind::kStringText:
        PutEscaped(ast_.string(expr.aux));
        break;

      case ExprKind::kCharLiteral:
        Put("'");
        PutEscapedCodePoint(expr.aux);
        Put("'");
        break;

      case ExprKind::kBoolLiteral:
        Put(expr.aux != 0 ? "true" : "false");
        break;

      case ExprKind::kPointerNull:
        Put("ptrnull");
        break;
      case ExprKind::kNullLiteral:
        Put("null");
        break;

      case ExprKind::kSelf:
        Put("self");
        break;

      case ExprKind::kPath:
        Put(Name(expr.name));
        break;

      case ExprKind::kUnary: {
        const bool parenthesize = context > Precedence::kUnary;
        if (parenthesize) {
          Put("(");
        }
        Put(TokenKindSpelling(expr.op));
        if (expr.op == TokenKind::kAmp &&
            HasFlag(expr.flags, ExprFlags::kMutable)) {
          Put("mut ");
        }
        EmitExpr(expr.lhs, Precedence::kUnary);
        if (parenthesize) {
          Put(")");
        }
        break;
      }

      case ExprKind::kAwait:
      case ExprKind::kSpawn: {
        const bool parenthesize = context > Precedence::kUnary;
        if (parenthesize) {
          Put("(");
        }
        Put(expr.kind == ExprKind::kAwait ? "wait " : "spawn ");
        EmitExpr(expr.lhs, Precedence::kUnary);
        if (parenthesize) {
          Put(")");
        }
        break;
      }

      case ExprKind::kBinary:
      case ExprKind::kAssign:
      case ExprKind::kRange: {
        const Precedence precedence = InfixPrecedence(expr.op);
        const bool parenthesize = precedence < context;
        if (parenthesize) {
          Put("(");
        }
        const bool right_associative = IsAssignmentOperator(expr.op);
        if (expr.kind == ExprKind::kRange) {
          // Either bound may be absent: `..b`, `a..` and a bare `..`.
          if (expr.lhs.is_valid()) {
            EmitExpr(expr.lhs, precedence);
          }
          Put(TokenKindSpelling(expr.op));
          if (expr.rhs.is_valid()) {
            EmitExpr(expr.rhs, Tighter(precedence));
          }
        } else {
          // Inside a dimension product `^` is a power, not a bitwise xor,
          // and `Length^2` is how a dimension is written -- including by the
          // diagnostic renderer, which the formatter should agree with.
          const bool power = in_dimension_ && expr.op == TokenKind::kCaret;
          EmitExpr(expr.lhs,
                   right_associative ? Tighter(precedence) : precedence);
          if (!power) {
            Put(" ");
          }
          Put(TokenKindSpelling(expr.op));
          if (!power) {
            Put(" ");
          }
          EmitExpr(expr.rhs,
                   right_associative ? precedence : Tighter(precedence));
        }
        if (parenthesize) {
          Put(")");
        }
        break;
      }

      case ExprKind::kCast: {
        const bool parenthesize = Precedence::kCast < context;
        if (parenthesize) {
          Put("(");
        }
        EmitExpr(expr.lhs, Precedence::kCast);
        Put(" as ");
        EmitType(expr.type);
        if (parenthesize) {
          Put(")");
        }
        break;
      }

      case ExprKind::kCall: {
        EmitExpr(expr.lhs, kPostfixContext);
        if (!expr.generic_args.empty()) {
          Put("<");
          bool first = true;
          for (TypeRefId arg : ast_.Get(expr.generic_args)) {
            if (!first) {
              Put(", ");
            }
            first = false;
            EmitType(arg);
          }
          Put(">");
        }
        EmitArguments(expr.args);
        break;
      }

      case ExprKind::kIndex:
        EmitExpr(expr.lhs, kPostfixContext);
        Put("[");
        EmitExpr(expr.rhs);
        Put("]");
        break;

      case ExprKind::kField:
      case ExprKind::kOptionalField:
        EmitExpr(expr.lhs, kPostfixContext);
        Put(expr.kind == ExprKind::kField ? "." : "?.");
        Put(Name(expr.name));
        break;

      case ExprKind::kTry:
        EmitExpr(expr.lhs, kPostfixContext);
        Put("?");
        break;

      case ExprKind::kStructLiteral: {
        EmitExpr(expr.lhs, kPostfixContext);
        Put(" {");
        bool first = true;
        for (ExprId field : ast_.Get(expr.args)) {
          Put(first ? " " : ", ");
          first = false;
          EmitExpr(field);
        }
        Put(first ? "}" : " }");
        break;
      }

      case ExprKind::kArrayLiteral:
        Put("[");
        EmitCommaSeparated(expr.args);
        Put("]");
        break;

      case ExprKind::kTupleLiteral:
        Put("(");
        EmitCommaSeparated(expr.args);
        Put(")");
        break;

      case ExprKind::kClosure: {
        const std::span<const ParamId> params = ast_.Get(expr.params);
        if (params.empty()) {
          Put("||");
        } else {
          Put("|");
          bool first = true;
          for (ParamId param : params) {
            if (!first) {
              Put(", ");
            }
            first = false;
            EmitParam(param);
          }
          Put("|");
        }
        if (expr.type.is_valid()) {
          Put(" -> ");
          EmitType(expr.type);
        }
        if (ast_.Contains(expr.lhs) &&
            ast_.Get(expr.lhs).kind == ExprKind::kBlock) {
          EmitBlockBody(expr.lhs);
        } else {
          Put(" ");
          EmitExpr(expr.lhs);
        }
        break;
      }

      case ExprKind::kBlock: {
        if (expr.stmts.empty() &&
            !HasCommentsIn(expr.span.begin, expr.span.end)) {
          Put("{}");
          break;
        }
        Put("{");
        NewLine();
        PushIndent();
        EmitStatements(expr);
        PopIndent();
        Put("}");
        break;
      }

      case ExprKind::kIf: {
        Put("if ");
        if (HasFlag(expr.flags, ExprFlags::kHasBinding)) {
          Put("const ");
          EmitPattern(expr.pattern);
          Put(" = ");
        }
        EmitExpr(expr.lhs);
        EmitBlockBody(expr.rhs);
        if (expr.extra.is_valid()) {
          Put(" else");
          if (ast_.Contains(expr.extra) &&
              ast_.Get(expr.extra).kind == ExprKind::kIf) {
            Put(" ");
            EmitExpr(expr.extra);
          } else {
            EmitBlockBody(expr.extra);
          }
        }
        break;
      }

      case ExprKind::kMatch: {
        Put("match ");
        EmitExpr(expr.lhs);
        Put(" {");
        NewLine();
        PushIndent();
        for (MatchArmId arm_id : ast_.Get(expr.arms)) {
          const MatchArm& arm = ast_.Get(arm_id);
          FlushCommentsBefore(arm.span.begin, true);
          bool first = true;
          for (PatternId pattern : ast_.Get(arm.patterns)) {
            if (!first) {
              Put(" | ");
            }
            first = false;
            EmitPattern(pattern);
          }
          if (arm.guard.is_valid()) {
            Put(" if ");
            EmitExpr(arm.guard);
          }
          Put(" =>");
          if (ast_.Contains(arm.body) &&
              ast_.Get(arm.body).kind == ExprKind::kBlock) {
            EmitBlockBody(arm.body);
          } else {
            Put(" ");
            EmitExpr(arm.body);
          }
          PutTrailingComment(arm.span.end);
          NewLine();
        }
        PopIndent();
        Put("}");
        break;
      }

      case ExprKind::kLoop:
        EmitLoopLabel(expr.label);
        Put("loop");
        EmitBlockBody(expr.rhs);
        break;

      case ExprKind::kWhile:
        EmitLoopLabel(expr.label);
        Put("while ");
        if (HasFlag(expr.flags, ExprFlags::kHasBinding)) {
          Put("const ");
          EmitPattern(expr.pattern);
          Put(" = ");
        }
        EmitExpr(expr.lhs);
        EmitBlockBody(expr.rhs);
        break;

      case ExprKind::kFor:
      case ExprKind::kForeach:
        EmitLoopLabel(expr.label);
        Put(expr.kind == ExprKind::kFor ? "for " : "foreach ");
        EmitPattern(expr.pattern);
        Put(" in ");
        EmitExpr(expr.lhs);
        EmitBlockBody(expr.rhs);
        break;

      case ExprKind::kUnsafe:
        Put("unsafe");
        EmitBlockBody(expr.lhs);
        break;

      case ExprKind::kRegion:
        Put("arena");
        EmitBlockBody(expr.lhs);
        break;

      case ExprKind::kError:
        Put("/* unparsable */");
        break;
    }
  }

  void EmitLoopLabel(Symbol label) {
    if (!label.is_valid()) {
      return;
    }
    Put(Name(label));
    Put(": ");
  }

  void PutNumberSuffix(const NumberLiteral& number) {
    if (number.suffix != NumericSuffix::kNone) {
      Put(NumericSuffixName(number.suffix));
    }
    if (number.has_unit()) {
      Put(Name(number.unit));
    }
  }

  void PutEscaped(std::string_view text) {
    for (char c : text) {
      switch (c) {
        case '"':
          Put("\\\"");
          break;
        case '\\':
          Put("\\\\");
          break;
        case '\n':
          Put("\\n");
          break;
        case '\t':
          Put("\\t");
          break;
        case '\r':
          Put("\\r");
          break;
        case '{':
          Put("\\{");
          break;
        case '\0':
          // A raw NUL in a source file is unreadable and truncates C
          // string consumers; the escape reads back the same (D230).
          Put("\\0");
          break;
        default:
          Put(std::string_view(&c, 1));
          break;
      }
    }
  }

  void PutEscapedCodePoint(uint32_t code_point) {
    switch (code_point) {
      case 0:
        Put("\\0");
        return;
      case '\n':
        Put("\\n");
        return;
      case '\t':
        Put("\\t");
        return;
      case '\r':
        Put("\\r");
        return;
      case '\'':
        Put("\\'");
        return;
      case '\\':
        Put("\\\\");
        return;
      default:
        break;
    }
    if (code_point < 0x80) {
      const char c = static_cast<char>(code_point);
      Put(std::string_view(&c, 1));
      return;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "\\u{%X}", code_point);
    Put(buffer);
  }

  void EmitCommaSeparated(AstRange<ExprId> items) {
    bool first = true;
    for (ExprId id : ast_.Get(items)) {
      if (!first) {
        Put(", ");
      }
      first = false;
      EmitExpr(id);
    }
  }

  void EmitArguments(AstRange<ExprId> args) {
    const std::string inline_form = Capture([&] {
      Put("(");
      EmitCommaSeparated(args);
      Put(")");
    });
    if (Fits(inline_form)) {
      Put(inline_form);
      return;
    }

    Put("(");
    NewLine();
    PushIndent();
    PushIndent();
    const std::span<const ExprId> ids = ast_.Get(args);
    for (size_t i = 0; i < ids.size(); ++i) {
      EmitExpr(ids[i]);
      if (i + 1 < ids.size()) {
        Put(",");
        NewLine();
      }
    }
    Put(")");
    PopIndent();
    PopIndent();
  }

  // -------------------------------------------------------------- patterns --

  void EmitPattern(PatternId id) {
    if (!ast_.Contains(id)) {
      Put("_");
      return;
    }
    const Pattern& pattern = ast_.Get(id);
    switch (pattern.kind) {
      case PatternKind::kWildcard:
        Put("_");
        return;
      case PatternKind::kRest:
        Put("..");
        return;
      case PatternKind::kBinding:
        if (HasFlag(pattern.flags, PatternFlags::kMutable)) {
          Put("mut ");
        }
        Put(Name(pattern.name));
        return;
      case PatternKind::kLiteral:
        EmitExpr(pattern.literal);
        return;
      case PatternKind::kTuple: {
        Put("(");
        bool first = true;
        for (PatternId element : ast_.Get(pattern.elements)) {
          if (!first) {
            Put(", ");
          }
          first = false;
          EmitPattern(element);
        }
        Put(")");
        return;
      }
      case PatternKind::kVariant: {
        PutPath(pattern.path);
        if (pattern.elements.empty()) {
          return;
        }
        Put("(");
        bool first = true;
        for (PatternId element : ast_.Get(pattern.elements)) {
          if (!first) {
            Put(", ");
          }
          first = false;
          EmitPattern(element);
        }
        Put(")");
        return;
      }
      case PatternKind::kStruct: {
        PutPath(pattern.path);
        Put(" {");
        bool first = true;
        for (PatternId element : ast_.Get(pattern.elements)) {
          Put(first ? " " : ", ");
          first = false;
          const Pattern& field = ast_.Get(element);
          Put(Name(field.field));
          // `{ x: x }` and `{ x }` mean the same thing, so the short form is
          // the canonical one.
          if (field.kind != PatternKind::kBinding ||
              !(field.name == field.field)) {
            Put(": ");
            EmitPattern(element);
          }
        }
        Put(first ? "}" : " }");
        return;
      }
      case PatternKind::kPresent: {
        const std::span<const PatternId> elements = ast_.Get(pattern.elements);
        if (!elements.empty()) {
          EmitPattern(elements.front());
        }
        Put("?");
        return;
      }
      case PatternKind::kError:
        Put("_");
        return;
    }
  }

  // ----------------------------------------------------------------- types --

  void EmitType(TypeRefId id) {
    if (!ast_.Contains(id)) {
      Put("_");
      return;
    }
    const TypeRef& type = ast_.Get(id);
    if (HasFlag(type.flags, TypeRefFlags::kSoa)) {
      Put("soa ");
    } else if (HasFlag(type.flags, TypeRefFlags::kAos)) {
      Put("aos ");
    }
    switch (type.kind) {
      case TypeRefKind::kPath: {
        PutPath(type.path);
        if (type.args.empty()) {
          return;
        }
        Put("<");
        bool first = true;
        for (TypeRefId arg : ast_.Get(type.args)) {
          if (!first) {
            Put(", ");
          }
          first = false;
          EmitType(arg);
        }
        Put(">");
        return;
      }
      case TypeRefKind::kOptional:
        EmitType(type.element);
        Put("?");
        return;
      case TypeRefKind::kFallible:
        EmitType(type.element);
        Put("!");
        if (type.args.size() == 1) {
          EmitType(ast_.Get(type.args)[0]);
        } else if (!type.args.empty()) {
          Put("(");
          bool first_error = true;
          for (TypeRefId error : ast_.Get(type.args)) {
            if (!first_error) {
              Put(", ");
            }
            first_error = false;
            EmitType(error);
          }
          Put(")");
        }
        return;
      case TypeRefKind::kSlice:
        Put("[]");
        EmitType(type.element);
        return;
      case TypeRefKind::kArray:
        Put("[");
        EmitExpr(type.length);
        Put("]");
        EmitType(type.element);
        return;
      case TypeRefKind::kFunction: {
        Put("fun(");
        bool first = true;
        for (TypeRefId arg : ast_.Get(type.args)) {
          if (!first) {
            Put(", ");
          }
          first = false;
          EmitType(arg);
        }
        Put(")");
        if (type.element.is_valid()) {
          Put(" -> ");
          EmitType(type.element);
        }
        return;
      }
      case TypeRefKind::kPointer:
        Put(HasFlag(type.flags, TypeRefFlags::kMutable) ? "*mut " : "*");
        EmitType(type.element);
        return;
      case TypeRefKind::kReference:
        Put(HasFlag(type.flags, TypeRefFlags::kMutable) ? "&mut " : "&");
        EmitType(type.element);
        return;
      case TypeRefKind::kTuple: {
        Put("(");
        bool first = true;
        for (TypeRefId arg : ast_.Get(type.args)) {
          if (!first) {
            Put(", ");
          }
          first = false;
          EmitType(arg);
        }
        Put(")");
        return;
      }
      case TypeRefKind::kMut:
        Put("mut ");
        EmitType(type.element);
        return;
      case TypeRefKind::kWith:
        Put("with ");
        EmitType(type.element);
        return;
      case TypeRefKind::kWithout:
        Put("without ");
        EmitType(type.element);
        return;
      case TypeRefKind::kError:
        Put("_");
        return;
    }
  }

  // Set while writing a dimension's product, where `^` is a power.
  bool in_dimension_ = false;

  const SourceManager& sources_;
  const StringInterner& interner_;
  const TokenStream& tokens_;
  const Ast& ast_;
  FormatOptions options_;
  std::string_view text_;

  std::string out_;
  int indent_ = 0;
  bool pending_indent_ = false;
  size_t comment_index_ = 0;
  int capturing_ = 0;
};

}  // namespace

std::string Format(const SourceManager& sources,
                   const StringInterner& interner,
                   const TokenStream& tokens,
                   const Ast& ast,
                   const FormatOptions& options) {
  Formatter formatter(sources, interner, tokens, ast, options);
  return formatter.Run();
}

}  // namespace wolang
