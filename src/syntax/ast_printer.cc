// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/syntax/ast_printer.h"

#include <cstdio>
#include <string_view>
#include <utility>

namespace wolang {
namespace {

std::string FormatDouble(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  return std::string(buffer);
}

std::string FormatUint64(uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu",
                static_cast<unsigned long long>(value));
  return std::string(buffer);
}

std::string Quote(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  out.push_back('"');
  return out;
}

class Printer {
 public:
  Printer(const SourceManager* sources,
          const StringInterner& interner,
          const Ast& ast,
          const AstPrintOptions& options)
      : sources_(sources),
        interner_(interner),
        ast_(ast),
        options_(options) {}

  std::string PrintFile() {
    Open(0, "file");
    for (ItemId id : ast_.Get(ast_.root())) {
      Break();
      PrintItem(id, static_cast<int>(options_.indent_width));
    }
    Close();
    out_.push_back('\n');
    return std::move(out_);
  }

  std::string PrintOne(ExprId expr) {
    PrintExprNode(expr, 0);
    out_.push_back('\n');
    return std::move(out_);
  }

 private:
  // Every Print* function writes its own indentation and leaves the cursor
  // right after its closing parenthesis, so a parent only has to emit a line
  // break between children.
  void Indent(int indent) { out_.append(static_cast<size_t>(indent), ' '); }
  void Break() { out_.push_back('\n'); }
  void Close() { out_.push_back(')'); }

  void Open(int indent, std::string_view head) {
    Indent(indent);
    out_.push_back('(');
    out_.append(head);
  }

  void Atom(std::string_view text) {
    out_.push_back(' ');
    out_.append(text);
  }

  void Name(Symbol symbol) {
    if (!symbol.is_valid()) {
      Atom("<missing>");
      return;
    }
    Atom(interner_.Text(symbol));
  }

  void Span(SourceSpan span) {
    if (!options_.include_spans || sources_ == nullptr ||
        !span.file.is_valid()) {
      return;
    }
    const LineColumn position = sources_->Position(span.file, span.begin);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "@%u:%u", position.line,
                  position.column);
    Atom(buffer);
  }

  std::string PathText(AstRange<Symbol> path) {
    std::string text;
    for (Symbol segment : ast_.Get(path)) {
      if (!text.empty()) {
        text.push_back('.');
      }
      text.append(interner_.Text(segment));
    }
    return text.empty() ? std::string("<missing>") : text;
  }

  int Step(int indent) const {
    return indent + static_cast<int>(options_.indent_width);
  }

  // ---------------------------------------------------------------- items --

  void PrintItem(ItemId id, int indent) {
    if (!ast_.Contains(id)) {
      Open(indent, "<invalid-item>");
      Close();
      return;
    }
    const Item& item = ast_.Get(id);
    const int child = Step(indent);

    std::string head(ItemKindName(item.kind));
    if (item.kind == ItemKind::kBinding) {
      head = HasFlag(item.flags, ItemFlags::kMutable) ? "var" : "const";
    }
    Open(indent, head);

    if (item.kind == ItemKind::kNeed) {
      Atom(PathText(item.path));
    } else if (item.name.is_valid()) {
      Name(item.name);
    }
    Span(item.span);

    PrintItemFlags(item.flags);

    for (AttributeId attribute : ast_.Get(item.attributes)) {
      Break();
      PrintAttribute(attribute, child);
    }

    if (!item.names.empty()) {
      Break();
      Open(child, "names");
      for (Symbol name : ast_.Get(item.names)) {
        Name(name);
      }
      Close();
    }

    if (item.kind == ItemKind::kUnit && !item.path.empty()) {
      Break();
      Open(child, "dimension");
      Atom(PathText(item.path));
      Close();
    }

    if (item.kind == ItemKind::kOn && !item.path.empty()) {
      Break();
      Open(child, "signal");
      Atom(PathText(item.path));
      Close();
    }

    for (GenericParamId generic : ast_.Get(item.generics)) {
      Break();
      PrintGenericParam(generic, child);
    }

    if (item.super_type.is_valid()) {
      Break();
      Open(child, HasFlag(item.flags, ItemFlags::kAbilityImpl) ? "as"
                                                             : "extends");
      Break();
      PrintType(item.super_type, Step(child));
      Close();
    }

    if (item.target_type.is_valid()) {
      Break();
      Open(child, "target");
      Break();
      PrintType(item.target_type, Step(child));
      Close();
    }

    if (!item.params.empty()) {
      Break();
      Open(child, "params");
      for (ParamId param : ast_.Get(item.params)) {
        Break();
        PrintParam(param, Step(child));
      }
      Close();
    }

    if (item.type.is_valid()) {
      Break();
      Open(child, item.kind == ItemKind::kFunction ? "returns" : "of");
      Break();
      PrintType(item.type, Step(child));
      Close();
    }

    if (!item.effects.empty()) {
      Break();
      Open(child, "uses");
      for (TypeRefId effect : ast_.Get(item.effects)) {
        Break();
        PrintType(effect, Step(child));
      }
      Close();
    }

    for (OrderingId ordering : ast_.Get(item.orderings)) {
      Break();
      const Ordering& node = ast_.Get(ordering);
      Open(child, node.is_after ? "after" : "before");
      Atom(PathText(node.path));
      Close();
    }

    if (item.link_name.is_valid()) {
      Break();
      Open(child, "symbol");
      Name(item.link_name);
      Close();
    }

    if (item.abi.is_valid()) {
      Break();
      Open(child, "abi");
      Name(item.abi);
      Close();
    }

    if (item.init.is_valid()) {
      Break();
      Open(child, HasFlag(item.flags, ItemFlags::kExpressionBody) ? "derive"
                                                                  : "init");
      Break();
      PrintExprNode(item.init, Step(child));
      Close();
    }

    for (StateId state : ast_.Get(item.states)) {
      Break();
      PrintState(state, child);
    }

    for (ItemId member : ast_.Get(item.members)) {
      Break();
      PrintItem(member, child);
    }

    if (item.body.is_valid()) {
      Break();
      PrintExprNode(item.body, child);
    }

    Close();
  }

  void PrintItemFlags(ItemFlags flags) {
    static constexpr struct {
      ItemFlags flag;
      std::string_view name;
    } kNames[] = {
        {ItemFlags::kOffer, "offer"},
        {ItemFlags::kPriv, "priv"},
        {ItemFlags::kAsync, "task"},
        {ItemFlags::kVirtual, "virtual"},
        {ItemFlags::kOverride, "override"},
        {ItemFlags::kComptime, "comptime"},
        {ItemFlags::kDerived, "derived"},
        {ItemFlags::kExtern, "extern"},
        {ItemFlags::kHasError, "has-error"},
    };
    for (const auto& entry : kNames) {
      if (HasFlag(flags, entry.flag)) {
        Atom(entry.name);
      }
    }
  }

  void PrintAttribute(AttributeId id, int indent) {
    const Attribute& attribute = ast_.Get(id);
    Open(indent, "attr");
    Name(attribute.name);
    for (ExprId arg : ast_.Get(attribute.args)) {
      Break();
      PrintExprNode(arg, Step(indent));
    }
    Close();
  }

  void PrintParam(ParamId id, int indent) {
    const Param& param = ast_.Get(id);
    Open(indent, "param");
    Name(param.name);
    if (param.is_mutable) {
      Atom("mut");
    }
    for (AttributeId attribute : ast_.Get(param.attributes)) {
      Break();
      PrintAttribute(attribute, Step(indent));
    }
    if (param.type.is_valid()) {
      Break();
      PrintType(param.type, Step(indent));
    }
    if (param.default_value.is_valid()) {
      Break();
      Open(Step(indent), "default");
      Break();
      PrintExprNode(param.default_value, Step(Step(indent)));
      Close();
    }
    Close();
  }

  void PrintGenericParam(GenericParamId id, int indent) {
    const GenericParam& param = ast_.Get(id);
    Open(indent, "generic");
    Name(param.name);
    for (TypeRefId bound : ast_.Get(param.bounds)) {
      Break();
      PrintType(bound, Step(indent));
    }
    Close();
  }

  void PrintState(StateId id, int indent) {
    const State& state = ast_.Get(id);
    const int child = Step(indent);
    Open(indent, "state");
    Name(state.name);
    if (state.enter.is_valid()) {
      Break();
      Open(child, "enter");
      Break();
      PrintExprNode(state.enter, Step(child));
      Close();
    }
    if (state.exit.is_valid()) {
      Break();
      Open(child, "exit");
      Break();
      PrintExprNode(state.exit, Step(child));
      Close();
    }
    for (TransitionId transition : ast_.Get(state.transitions)) {
      Break();
      const Transition& node = ast_.Get(transition);
      Open(child, "transition");
      if (node.is_always) {
        Atom("always");
      } else {
        Name(node.event);
      }
      Atom("->");
      Name(node.target);
      if (node.delay.is_valid()) {
        Break();
        Open(Step(child), "after");
        Break();
        PrintExprNode(node.delay, Step(Step(child)));
        Close();
      }
      Close();
    }
    Close();
  }

  // ----------------------------------------------------------- statements --

  void PrintStmt(StmtId id, int indent) {
    if (!ast_.Contains(id)) {
      Open(indent, "<invalid-stmt>");
      Close();
      return;
    }
    const Stmt& stmt = ast_.Get(id);
    const int child = Step(indent);
    Open(indent, StmtKindName(stmt.kind));
    if (stmt.label.is_valid()) {
      Name(stmt.label);
    }
    Span(stmt.span);

    if (stmt.pattern.is_valid()) {
      Break();
      PrintPattern(stmt.pattern, child);
    }
    if (stmt.type.is_valid()) {
      Break();
      PrintType(stmt.type, child);
    }
    if (stmt.item.is_valid()) {
      Break();
      PrintItem(stmt.item, child);
    }
    if (stmt.expr.is_valid()) {
      Break();
      PrintExprNode(stmt.expr, child);
    }
    Close();
  }

  // ---------------------------------------------------------- expressions --

  void PrintExprNode(ExprId id, int indent) {
    if (!ast_.Contains(id)) {
      Open(indent, "<invalid-expr>");
      Close();
      return;
    }
    const Expr& expr = ast_.Get(id);
    const int child = Step(indent);
    Open(indent, ExprKindName(expr.kind));

    switch (expr.kind) {
      case ExprKind::kIntLiteral: {
        const NumberLiteral& number = ast_.number(expr.aux);
        Atom(FormatUint64(number.integer_value));
        PrintNumberSuffix(number);
        break;
      }
      case ExprKind::kFloatLiteral: {
        const NumberLiteral& number = ast_.number(expr.aux);
        Atom(FormatDouble(number.float_value));
        PrintNumberSuffix(number);
        break;
      }
      case ExprKind::kStringText:
        Atom(Quote(ast_.string(expr.aux)));
        break;
      case ExprKind::kCharLiteral:
        Atom(FormatUint64(expr.aux));
        break;
      case ExprKind::kBoolLiteral:
        Atom(expr.aux != 0 ? "true" : "false");
        break;
      case ExprKind::kPath:
      case ExprKind::kField:
      case ExprKind::kOptionalField:
        Name(expr.name);
        break;
      case ExprKind::kUnary:
      case ExprKind::kBinary:
      case ExprKind::kAssign:
        Atom(TokenKindSpelling(expr.op));
        break;
      case ExprKind::kRange:
        Atom(HasFlag(expr.flags, ExprFlags::kInclusive) ? "..=" : "..");
        break;
      default:
        break;
    }

    if (expr.label.is_valid()) {
      std::string text = "label=";
      text.append(interner_.Text(expr.label));
      Atom(text);
    }
    if (HasFlag(expr.flags, ExprFlags::kMutable)) {
      Atom("mut");
    }
    if (HasFlag(expr.flags, ExprFlags::kHasError)) {
      Atom("has-error");
    }
    Span(expr.span);

    if (expr.pattern.is_valid()) {
      Break();
      PrintPattern(expr.pattern, child);
    }

    if (!expr.params.empty()) {
      Break();
      Open(child, "params");
      for (ParamId param : ast_.Get(expr.params)) {
        Break();
        PrintParam(param, Step(child));
      }
      Close();
    }

    if (expr.kind == ExprKind::kRange) {
      // A bound may be absent on either side, so each one is labelled: an
      // unlabelled child would make `2..` and `..5` print identically.
      if (expr.lhs.is_valid()) {
        Break();
        Open(child, "from");
        Break();
        PrintExprNode(expr.lhs, Step(child));
        Close();
      }
      if (expr.rhs.is_valid()) {
        Break();
        Open(child, "to");
        Break();
        PrintExprNode(expr.rhs, Step(child));
        Close();
      }
      Close();
      return;
    }

    if (expr.lhs.is_valid()) {
      Break();
      PrintExprNode(expr.lhs, child);
    }

    if (!expr.generic_args.empty()) {
      Break();
      Open(child, "generics");
      for (TypeRefId arg : ast_.Get(expr.generic_args)) {
        Break();
        PrintType(arg, Step(child));
      }
      Close();
    }

    if (!expr.args.empty()) {
      const bool is_argument_list =
          expr.kind == ExprKind::kCall || expr.kind == ExprKind::kStructLiteral;
      if (is_argument_list) {
        Break();
        Open(child, "args");
        for (ExprId arg : ast_.Get(expr.args)) {
          Break();
          PrintExprNode(arg, Step(child));
        }
        Close();
      } else {
        for (ExprId arg : ast_.Get(expr.args)) {
          Break();
          PrintExprNode(arg, child);
        }
      }
    }

    if (expr.rhs.is_valid()) {
      Break();
      PrintExprNode(expr.rhs, child);
    }

    if (expr.type.is_valid()) {
      Break();
      PrintType(expr.type, child);
    }

    for (StmtId stmt : ast_.Get(expr.stmts)) {
      Break();
      PrintStmt(stmt, child);
    }

    for (MatchArmId arm : ast_.Get(expr.arms)) {
      Break();
      PrintMatchArm(arm, child);
    }

    if (expr.extra.is_valid()) {
      Break();
      Open(child, "else");
      Break();
      PrintExprNode(expr.extra, Step(child));
      Close();
    }

    Close();
  }

  void PrintNumberSuffix(const NumberLiteral& number) {
    if (number.suffix != NumericSuffix::kNone) {
      std::string text = "suffix=";
      text.append(NumericSuffixName(number.suffix));
      Atom(text);
    }
    if (number.has_unit()) {
      std::string text = "unit=";
      text.append(interner_.Text(number.unit));
      Atom(text);
    }
  }

  void PrintMatchArm(MatchArmId id, int indent) {
    const MatchArm& arm = ast_.Get(id);
    const int child = Step(indent);
    Open(indent, "arm");
    for (PatternId pattern : ast_.Get(arm.patterns)) {
      Break();
      PrintPattern(pattern, child);
    }
    if (arm.guard.is_valid()) {
      Break();
      Open(child, "guard");
      Break();
      PrintExprNode(arm.guard, Step(child));
      Close();
    }
    if (arm.body.is_valid()) {
      Break();
      PrintExprNode(arm.body, child);
    }
    Close();
  }

  // ------------------------------------------------------------- patterns --

  void PrintPattern(PatternId id, int indent) {
    if (!ast_.Contains(id)) {
      Open(indent, "<invalid-pattern>");
      Close();
      return;
    }
    const Pattern& pattern = ast_.Get(id);
    Open(indent, PatternKindName(pattern.kind));

    if (!pattern.path.empty()) {
      Atom(PathText(pattern.path));
    } else if (pattern.name.is_valid()) {
      Name(pattern.name);
    }
    if (pattern.field.is_valid()) {
      std::string text = "field=";
      text.append(interner_.Text(pattern.field));
      Atom(text);
    }
    if (HasFlag(pattern.flags, PatternFlags::kMutable)) {
      Atom("mut");
    }
    Span(pattern.span);

    if (pattern.literal.is_valid()) {
      Break();
      PrintExprNode(pattern.literal, Step(indent));
    }
    for (PatternId element : ast_.Get(pattern.elements)) {
      Break();
      PrintPattern(element, Step(indent));
    }
    Close();
  }

  // ---------------------------------------------------------------- types --

  void PrintType(TypeRefId id, int indent) {
    if (!ast_.Contains(id)) {
      Open(indent, "<invalid-type>");
      Close();
      return;
    }
    const TypeRef& type = ast_.Get(id);
    Open(indent, TypeRefKindName(type.kind));

    if (type.kind == TypeRefKind::kPath) {
      Atom(PathText(type.path));
    }
    if (HasFlag(type.flags, TypeRefFlags::kSoa)) {
      Atom("soa");
    }
    if (HasFlag(type.flags, TypeRefFlags::kAos)) {
      Atom("aos");
    }
    if (HasFlag(type.flags, TypeRefFlags::kMutable) &&
        type.kind != TypeRefKind::kMut) {
      Atom("mut");
    }
    Span(type.span);

    for (TypeRefId arg : ast_.Get(type.args)) {
      Break();
      PrintType(arg, Step(indent));
    }
    if (type.element.is_valid()) {
      Break();
      PrintType(type.element, Step(indent));
    }
    if (type.length.is_valid()) {
      Break();
      Open(Step(indent), "length");
      Break();
      PrintExprNode(type.length, Step(Step(indent)));
      Close();
    }
    Close();
  }

  const SourceManager* sources_;
  const StringInterner& interner_;
  const Ast& ast_;
  AstPrintOptions options_;
  std::string out_;
};

}  // namespace

std::string PrintAst(const SourceManager* sources,
                     const StringInterner& interner,
                     const Ast& ast,
                     const AstPrintOptions& options) {
  Printer printer(sources, interner, ast, options);
  return printer.PrintFile();
}

std::string PrintExpr(const SourceManager* sources,
                      const StringInterner& interner,
                      const Ast& ast,
                      ExprId expr,
                      const AstPrintOptions& options) {
  Printer printer(sources, interner, ast, options);
  return printer.PrintOne(expr);
}

}  // namespace wolang
