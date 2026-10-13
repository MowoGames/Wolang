// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/type_check.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace wolang {
namespace {

constexpr std::string_view kMismatchedTypes = "E0101";
constexpr std::string_view kWrongArguments = "E0102";
constexpr std::string_view kUnknownName = "E0103";
constexpr std::string_view kMaybeNull = "E0104";
constexpr std::string_view kAssignToLet = "E0105";
constexpr std::string_view kNotCallable = "E0106";
constexpr std::string_view kNotIndexable = "E0107";
constexpr std::string_view kUnknownMember = "E0112";
constexpr std::string_view kNonExhaustive = "E0201";
constexpr std::string_view kMissingTraitMethod = "E0402";
constexpr std::string_view kBadOperand = "E0108";
constexpr std::string_view kLiteralOutOfRange = "E0109";
constexpr std::string_view kDimensionMismatch = "E1001";
constexpr std::string_view kUnknownUnit = "E1002";

// One lexical scope: locals shadow whatever is outside.
struct Scope {
  std::vector<Binding> bindings;
};

// The range an integer type can hold, used to reject a literal that does not
// fit its destination rather than wrapping it silently.
struct IntRange {
  bool has_range = false;
  uint64_t max_magnitude = 0;
  bool is_signed = true;
};

IntRange RangeOf(const TypeStore& types, TypeId id) {
  if (types.KindOf(id) != TypeKind::kInt) {
    return IntRange{};
  }
  const uint32_t bits = types.BitWidth(id);
  const bool is_signed = types.Get(id).is_signed;
  IntRange range;
  range.has_range = true;
  range.is_signed = is_signed;
  // For a signed type the magnitude bound is 2^(n-1); the extra value on the
  // negative side is handled where the sign is known.
  range.max_magnitude = is_signed
                            ? (uint64_t{1} << (bits - 1))
                            : (bits == 64 ? UINT64_MAX
                                          : (uint64_t{1} << bits) - 1);
  return range;
}

class Checker {
 public:
  Checker(TypeStore* types,
          const StringInterner* interner,
          const FileResolution* resolution,
          const Ast& ast)
      : types_(types),
        interner_(interner),
        resolution_(resolution),
        ast_(ast),
        resolver_(types, interner, resolution, &diagnostics_) {
    result_.expr_types.assign(ast.expr_count(), TypeId{});
  }

  TypedBody Run(ItemId item);

 private:
  // ----------------------------------------------------------- reporting --

  void Report(std::string_view code,
              std::string message,
              SourceSpan span,
              std::string label = {}) {
    diagnostics_.Error(code, std::move(message))
        .Primary(span, std::move(label));
  }

  // Passed to Dimensions::Describe so a product prints with its declared
  // names: dimension ids are global, so the type store knows them all.
  std::function<Symbol(DimensionId)> DimensionName() const {
    return [this](DimensionId d) { return types_->DimensionName(d); };
  }

  std::string Describe(TypeId id) const {
    return types_->Describe(*interner_, id);
  }

  // Reports a mismatch unless either side is already an error type, which is
  // what stops one real problem from producing a cascade.
  void ReportMismatch(TypeId expected, TypeId found, SourceSpan span) {
    if (types_->IsError(expected) || types_->IsError(found)) {
      return;
    }
    std::string message = "expected `";
    message.append(Describe(expected));
    message.append("`, found `");
    message.append(Describe(found));
    message.append("`");
    Report(kMismatchedTypes, std::move(message), span);
  }

  // -------------------------------------------------------------- scopes --

  void PushScope() { scopes_.emplace_back(); }
  void PopScope() {
    if (!scopes_.empty()) {
      scopes_.pop_back();
    }
  }
  void Bind(Symbol name, TypeId type, bool is_mutable);
  const Binding* Lookup(Symbol name) const;

  // --------------------------------------------------------- declarations --

  void CheckItem(ItemId id);
  void CheckFunction(const Item& item);
  void CheckBinding(const Item& item);

  // ---------------------------------------------------------- statements --

  void CheckBlock(ExprId id, TypeId expected);
  void CheckStmt(StmtId id);
  // |is_mutable| comes from the `var` keyword of the enclosing
  // statement; a pattern of its own only says `mut`.
  void BindPattern(PatternId id, TypeId type,
                   bool is_mutable = false);

  // --------------------------------------------------------- expressions --

  // |expected| is the type the context wants, or an invalid id when it has no
  // opinion. The returned type is what the expression actually has.
  //
  // |keep_open| leaves a literal un-defaulted. A binary operator uses it for
  // both operands, because in `1 + small` neither side should settle before
  // the other has been seen.
  TypeId Check(ExprId id, TypeId expected, bool keep_open = false);
  TypeId CheckLiteral(const Expr& expr, TypeId expected);
  TypeId CheckPath(const Expr& expr);
  TypeId CheckUnary(const Expr& expr, TypeId expected);
  TypeId CheckBinary(const Expr& expr);
  TypeId CheckAssign(const Expr& expr);
  TypeId CheckCall(const Expr& expr);
  TypeId CheckIndex(const Expr& expr);
  TypeId CheckField(const Expr& expr);
  void CheckConstructor(const Expr& expr, TypeId type);

  // The member of the enclosing type named |name|, or null when there is no
  // enclosing type or it has no such member.
  const Member* SelfMember(Symbol name) const;
  TypeId CheckIf(const Expr& expr, TypeId expected);
  TypeId CheckMatch(const Expr& expr, TypeId expected);
  void CheckExhaustive(const Expr& expr, TypeId subject);
  void CheckTraitConformance(const Item& item);
  TypeId CheckLoop(const Expr& expr);
  TypeId CheckRange(const Expr& expr);

  // Adjusts a literal to the type its context wants, and reports when the
  // value does not fit.
  TypeId Adapt(TypeId found, TypeId expected, SourceSpan span);
  // True when a value of |found| may be used where |expected| is wanted.
  bool Accepts(TypeId expected, TypeId found) const;
  // The type of a numeric literal with a unit suffix.
  TypeId QuantityOfUnit(const NumberLiteral& number,
                        TypeId numeric,
                        SourceSpan span);

  void Record(ExprId id, TypeId type) {
    if (id.is_valid() && id.index < result_.expr_types.size()) {
      result_.expr_types[id.index] = type;
    }
  }

  TypeStore* types_;
  const StringInterner* interner_;
  const FileResolution* resolution_;
  const Ast& ast_;
  DiagnosticEngine diagnostics_;
  TypeResolver resolver_;
  TypedBody result_;
  std::vector<Scope> scopes_;
  // The declared return type of the function being checked.
  TypeId return_type_;
  // The type whose members are in scope, inside a method.
  TypeId self_type_;
  // Nesting depth of loops, so `break` outside one can be reported.
  uint32_t loop_depth_ = 0;
};

void Checker::Bind(Symbol name, TypeId type, bool is_mutable) {
  if (scopes_.empty() || !name.is_valid()) {
    return;
  }
  Binding binding;
  binding.name = name;
  binding.kind = BindingKind::kLocal;
  binding.type = type;
  binding.is_mutable = is_mutable;
  scopes_.back().bindings.push_back(binding);
}

const Binding* Checker::Lookup(Symbol name) const {
  // Innermost scope first, so a local shadows a field and a field shadows a
  // module-level name.
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    for (auto it = scope->bindings.rbegin(); it != scope->bindings.rend();
         ++it) {
      if (it->name == name) {
        return &*it;
      }
    }
  }
  return resolution_ == nullptr ? nullptr : resolution_->Find(name);
}

// ------------------------------------------------------------- adaptation ---

bool Checker::Accepts(TypeId expected, TypeId found) const {
  if (!expected.is_valid() || !found.is_valid()) {
    return true;
  }
  // An expectation that is itself an open literal says nothing: it is what a
  // sibling operand happened to be before either side settled.
  if (types_->IsLiteral(expected)) {
    return types_->IsNumeric(found);
  }
  if (expected == found) {
    return true;
  }
  // An error anywhere is already reported; accepting it keeps the cascade
  // from starting.
  if (types_->IsError(expected) || types_->IsError(found)) {
    return true;
  }
  // `never` is the type of an expression that does not finish, so it fits
  // wherever a value is wanted.
  if (types_->KindOf(found) == TypeKind::kNever) {
    return true;
  }
  // A literal adapts to any numeric type of the right family.
  if (types_->KindOf(found) == TypeKind::kIntLiteral) {
    return types_->IsNumeric(expected);
  }
  if (types_->KindOf(found) == TypeKind::kFloatLiteral) {
    return types_->IsFloatingPoint(expected) ||
           types_->KindOf(expected) == TypeKind::kQuantity;
  }
  // Anything fits an optional of itself, which is what makes `let x: Pawn? =
  // pawn` work without a wrapper call.
  if (types_->IsOptional(expected)) {
    return Accepts(types_->ElementOf(expected), found);
  }
  return false;
}

TypeId Checker::Adapt(TypeId found, TypeId expected, SourceSpan span) {
  if (expected.is_valid() && types_->IsLiteral(expected)) {
    // The expectation was an open literal, so it imposes nothing; the found
    // type wins and stays open if it is a literal too.
    return found;
  }
  if (!expected.is_valid()) {
    // No expectation: a literal settles on its default type.
    if (types_->KindOf(found) == TypeKind::kIntLiteral) {
      return types_->default_int();
    }
    if (types_->KindOf(found) == TypeKind::kFloatLiteral) {
      return types_->default_float();
    }
    return found;
  }

  if (!Accepts(expected, found)) {
    ReportMismatch(expected, found, span);
    return expected;
  }

  if (types_->IsLiteral(found)) {
    // An integer literal in a float context is fine; the reverse is not,
    // which Accepts already decided.
    if (types_->KindOf(expected) == TypeKind::kQuantity) {
      return expected;
    }
    return expected;
  }
  if (types_->IsOptional(expected) && !types_->IsOptional(found)) {
    return expected;
  }
  return found;
}

TypeId Checker::QuantityOfUnit(const NumberLiteral& number,
                               TypeId numeric,
                               SourceSpan span) {
  const Binding* unit = Lookup(number.unit);
  if (unit == nullptr || unit->kind != BindingKind::kUnit) {
    std::string message = "no unit named `";
    message.append(interner_->Text(number.unit));
    message.append("` is in scope");
    Report(kUnknownUnit, std::move(message), span,
           "declare it with `unit s: Time`");
    return types_->error();
  }
  // A unit literal is always a float: `200ms` is 0.2 seconds, and keeping it
  // integral would make that conversion lossy.
  return types_->Quantity(numeric,
                          Dimensions::Base(unit->dimension));
}

// -------------------------------------------------------------- literals ----

TypeId Checker::CheckLiteral(const Expr& expr, TypeId expected) {
  switch (expr.kind) {
    case ExprKind::kIntLiteral:
    case ExprKind::kFloatLiteral: {
      const NumberLiteral& number = ast_.number(expr.aux);
      const bool is_float = expr.kind == ExprKind::kFloatLiteral;

      if (number.has_unit()) {
        const TypeId numeric = types_->default_float();
        return QuantityOfUnit(number, numeric, expr.span);
      }

      // An explicit suffix fixes the type; otherwise the literal stays open
      // and takes whatever the context wants.
      switch (number.suffix) {
        case NumericSuffix::kI8:
          return types_->Int(IntWidth::k8, true);
        case NumericSuffix::kI16:
          return types_->Int(IntWidth::k16, true);
        case NumericSuffix::kI32:
          return types_->Int(IntWidth::k32, true);
        case NumericSuffix::kI64:
          return types_->Int(IntWidth::k64, true);
        case NumericSuffix::kU8:
          return types_->Int(IntWidth::k8, false);
        case NumericSuffix::kU16:
          return types_->Int(IntWidth::k16, false);
        case NumericSuffix::kU32:
          return types_->Int(IntWidth::k32, false);
        case NumericSuffix::kU64:
          return types_->Int(IntWidth::k64, false);
        case NumericSuffix::kF32:
          return types_->Float(FloatWidth::k32);
        case NumericSuffix::kF64:
          return types_->Float(FloatWidth::k64);
        case NumericSuffix::kNone:
          break;
      }

      if (is_float) {
        return types_->float_literal();
      }

      // An integer literal has arbitrary precision until it is used, so the
      // only thing to check here is that it fits where it is going.
      const TypeId target =
          expected.is_valid() && types_->KindOf(expected) == TypeKind::kInt
              ? expected
              : TypeId{};
      if (target.is_valid()) {
        const IntRange range = RangeOf(*types_, target);
        const uint64_t value = number.integer_value;
        const bool fits = range.is_signed ? value <= range.max_magnitude
                                          : value <= range.max_magnitude;
        if (!fits) {
          std::string message = "the literal does not fit in `";
          message.append(Describe(target));
          message.append("`");
          Report(kLiteralOutOfRange, std::move(message), expr.span);
          return target;
        }
      }
      return types_->int_literal();
    }

    case ExprKind::kStringLiteral:
      for (ExprId part : ast_.Get(expr.args)) {
        const Expr& piece = ast_.Get(part);
        if (piece.kind != ExprKind::kStringText) {
          // An interpolated expression can be anything that formats; its own
          // type is checked without an expectation.
          Check(part, TypeId{});
        } else {
          Record(part, types_->str());
        }
      }
      return types_->string_type();

    case ExprKind::kCharLiteral:
      return types_->char_type();
    case ExprKind::kBoolLiteral:
      return types_->bool_type();
    case ExprKind::kNullLiteral:
      // `null` alone has no type; it takes the optional the context wants.
      if (expected.is_valid() && types_->IsOptional(expected)) {
        return expected;
      }
      return types_->Optional(types_->never());

    default:
      return types_->error();
  }
}

// ------------------------------------------------------------ expressions ---

const Member* Checker::SelfMember(Symbol name) const {
  if (!self_type_.is_valid()) {
    return nullptr;
  }
  return resolution_->FindMember(types_->StripWrappers(self_type_), name);
}

TypeId Checker::CheckPath(const Expr& expr) {
  const Binding* binding = Lookup(expr.name);
  if (binding == nullptr) {
    // Inside a method a bare name may be a member of the enclosing type:
    // `health` means `self.health`, which is how gameplay code reads.
    if (const Member* member = SelfMember(expr.name); member != nullptr) {
      return member->type;
    }
    std::string message = "no value named `";
    message.append(interner_->Text(expr.name));
    message.append("` is in scope");
    Report(kUnknownName, std::move(message), expr.span);
    return types_->error();
  }
  if (binding->kind == BindingKind::kErrorTag) {
    // An error tag is a value of the error set it belongs to.
    return types_->ErrorSet({binding->name});
  }
  return binding->type.is_valid() ? binding->type : types_->error();
}

TypeId Checker::CheckUnary(const Expr& expr, TypeId expected) {
  const TypeId operand = Check(expr.lhs, expected);
  switch (expr.op) {
    case TokenKind::kMinus:
      if (!types_->IsNumeric(operand) && !types_->IsError(operand)) {
        std::string message = "`-` needs a number, found `";
        message.append(Describe(operand));
        message.append("`");
        Report(kBadOperand, std::move(message), expr.span);
        return types_->error();
      }
      return operand;
    case TokenKind::kBang:
      if (!Accepts(types_->bool_type(), operand)) {
        ReportMismatch(types_->bool_type(), operand, expr.span);
      }
      return types_->bool_type();
    case TokenKind::kTilde:
      if (!types_->IsInteger(operand) && !types_->IsError(operand)) {
        std::string message = "`~` needs an integer, found `";
        message.append(Describe(operand));
        message.append("`");
        Report(kBadOperand, std::move(message), expr.span);
        return types_->error();
      }
      return operand;
    case TokenKind::kAmp:
      return types_->Reference(operand,
                               HasFlag(expr.flags, ExprFlags::kMutable));
    case TokenKind::kStar: {
      const TypeId target = types_->ElementOf(operand);
      if (!target.is_valid()) {
        std::string message = "cannot dereference `";
        message.append(Describe(operand));
        message.append("`");
        Report(kBadOperand, std::move(message), expr.span);
        return types_->error();
      }
      return target;
    }
    default:
      return operand;
  }
}

TypeId Checker::CheckBinary(const Expr& expr) {
  const TypeId left = Check(expr.lhs, TypeId{}, /*keep_open=*/true);

  // The left side's type is normally the expectation for the right, which is
  // what makes `health - 1` give the literal the field's type. Three
  // exceptions:
  //   * quantities, because `distance / duration` is supposed to mix
  //     dimensions and `10m + 2s` must reach the dimension check rather than
  //     being reported as a plain mismatch;
  //   * `??`, whose right side matches what is *inside* the optional;
  //   * the boolean operators, which want `bool` on both sides.
  TypeId right_expectation = left;
  if (types_->KindOf(left) == TypeKind::kQuantity) {
    right_expectation = TypeId{};
  } else if (expr.op == TokenKind::kQuestionQuestion) {
    right_expectation = types_->IsOptional(left) ? types_->ElementOf(left)
                                                 : TypeId{};
  } else if (expr.op == TokenKind::kAmpAmp ||
             expr.op == TokenKind::kPipePipe) {
    right_expectation = types_->bool_type();
  }
  const TypeId right =
      Check(expr.rhs, right_expectation, /*keep_open=*/true);

  switch (expr.op) {
    case TokenKind::kAmpAmp:
    case TokenKind::kPipePipe:
      if (!Accepts(types_->bool_type(), left)) {
        ReportMismatch(types_->bool_type(), left, expr.span);
      }
      if (!Accepts(types_->bool_type(), right)) {
        ReportMismatch(types_->bool_type(), right, expr.span);
      }
      return types_->bool_type();

    case TokenKind::kEqEq:
    case TokenKind::kBangEq:
    case TokenKind::kLess:
    case TokenKind::kLessEq:
    case TokenKind::kGreater:
    case TokenKind::kGreaterEq: {
      const Dimensions left_dimensions = types_->DimensionsOf(left);
      const Dimensions right_dimensions = types_->DimensionsOf(right);
      if (left_dimensions != right_dimensions) {
        // Comparing metres with seconds is the same bug as adding them.
        std::string message = "cannot compare `";
        message.append(
            left_dimensions.Describe(*interner_, DimensionName()));
        message.append("` with `");
        message.append(
            right_dimensions.Describe(*interner_, DimensionName()));
        message.append("`");
        Report(kDimensionMismatch, std::move(message), expr.span);
      } else if (!Accepts(left, right) && !Accepts(right, left)) {
        ReportMismatch(left, right, expr.span);
      }
      return types_->bool_type();
    }

    case TokenKind::kQuestionQuestion: {
      // `a ?? b` removes one layer of optional, and `b` has to fit what is
      // inside.
      if (!types_->IsOptional(left)) {
        if (!types_->IsError(left)) {
          std::string message = "`??` needs an optional on its left, found `";
          message.append(Describe(left));
          message.append("`");
          Report(kBadOperand, std::move(message), expr.span);
        }
        return left;
      }
      const TypeId inner = types_->ElementOf(left);
      if (!Accepts(inner, right)) {
        ReportMismatch(inner, right, expr.span);
      }
      return inner;
    }

    case TokenKind::kStar:
    case TokenKind::kSlash: {
      // Multiplying and dividing quantities combines their dimensions, which
      // is what gives `10m / 2s` a type nobody declared.
      const bool left_quantity =
          types_->KindOf(left) == TypeKind::kQuantity;
      const bool right_quantity =
          types_->KindOf(right) == TypeKind::kQuantity;
      if (left_quantity || right_quantity) {
        const Dimensions left_dimensions = types_->DimensionsOf(left);
        const Dimensions right_dimensions = types_->DimensionsOf(right);
        const Dimensions combined =
            expr.op == TokenKind::kStar
                ? left_dimensions.Multiply(right_dimensions)
                : left_dimensions.Divide(right_dimensions);
        const TypeId carrier = left_quantity ? types_->ElementOf(left)
                                             : types_->ElementOf(right);
        return types_->Quantity(carrier, combined);
      }
      [[fallthrough]];
    }

    case TokenKind::kPlus:
    case TokenKind::kMinus:
    case TokenKind::kPercent: {
      if (!types_->IsNumeric(left) || !types_->IsNumeric(right)) {
        if (!types_->IsError(left) && !types_->IsError(right)) {
          std::string message = "`";
          message.append(TokenKindSpelling(expr.op));
          message.append("` needs numbers, found `");
          message.append(Describe(left));
          message.append("` and `");
          message.append(Describe(right));
          message.append("`");
          Report(kBadOperand, std::move(message), expr.span);
        }
        return types_->error();
      }
      // Adding quantities requires the same dimension: metres plus seconds
      // is the bug the whole feature exists to catch.
      const Dimensions left_dimensions = types_->DimensionsOf(left);
      const Dimensions right_dimensions = types_->DimensionsOf(right);
      if (left_dimensions != right_dimensions) {
        std::string message = "cannot combine `";
        message.append(
            left_dimensions.Describe(*interner_, DimensionName()));
        message.append("` with `");
        message.append(
            right_dimensions.Describe(*interner_, DimensionName()));
        message.append("`");
        Report(kDimensionMismatch, std::move(message), expr.span,
               "both sides of `+` and `-` need the same dimension");
        return left;
      }
      // A literal on either side takes the other side's type; when both are
      // literals the result stays open for the enclosing context to decide.
      return types_->IsLiteral(left) ? right : left;
    }

    default:
      return types_->IsLiteral(left) ? right : left;
  }
}

TypeId Checker::CheckAssign(const Expr& expr) {
  const TypeId target = Check(expr.lhs, TypeId{});

  // Only a name, a field or an element can be assigned to, and a `let`
  // binding cannot be reassigned at all.
  if (ast_.Contains(expr.lhs)) {
    const Expr& left = ast_.Get(expr.lhs);
    if (left.kind == ExprKind::kPath) {
      const Binding* binding = Lookup(left.name);
      const Member* member =
          binding == nullptr ? SelfMember(left.name) : nullptr;
      if (member != nullptr && !member->is_mutable) {
        std::string message = "cannot assign to `";
        message.append(interner_->Text(left.name));
        message.append("`, which is not `var`");
        Report(kAssignToLet, std::move(message), expr.span);
      }
      if (binding != nullptr && !binding->is_mutable &&
          (binding->kind == BindingKind::kLocal ||
           binding->kind == BindingKind::kValue)) {
        std::string message = "cannot assign to `";
        message.append(interner_->Text(left.name));
        message.append("`, which is not `var`");
        Report(kAssignToLet, std::move(message), expr.span);
      }
    }
  }

  const TypeId value = Check(expr.rhs, target);
  if (!Accepts(target, value)) {
    ReportMismatch(target, value, expr.span);
  }
  return types_->void_type();
}

TypeId Checker::CheckCall(const Expr& expr) {
  // A type name used as a callee is a constructor: `Vec3()` produces a Vec3.
  // Field types arrive with member resolution, so the arguments are checked
  // without expectations until then.
  if (ast_.Contains(expr.lhs)) {
    const Expr& callee_expr = ast_.Get(expr.lhs);
    if (callee_expr.kind == ExprKind::kPath) {
      const Binding* binding = Lookup(callee_expr.name);
      if (binding != nullptr && binding->kind == BindingKind::kType) {
        CheckConstructor(expr, binding->type);
        Record(expr.lhs, binding->type);
        return binding->type;
      }
    }
  }

  const TypeId callee = Check(expr.lhs, TypeId{});
  const std::span<const ExprId> args = ast_.Get(expr.args);

  if (types_->KindOf(callee) != TypeKind::kFunction) {
    // The callee's type is unknown until member resolution lands, so an
    // unresolved call checks its arguments and yields an error type rather
    // than inventing a signature.
    for (ExprId arg : args) {
      Check(arg, TypeId{});
    }
    if (!types_->IsError(callee)) {
      std::string message = "`";
      message.append(Describe(callee));
      message.append("` is not callable");
      Report(kNotCallable, std::move(message), expr.span);
    }
    return types_->error();
  }

  std::vector<TypeId> params = types_->ArgumentsOf(callee);
  TypeId result = types_->ElementOf(callee);

  // A generic signature has its parameters inferred from the arguments:
  // `First(xs)` works out `T` from the type of `xs`, so no turbofish and no
  // explicit argument are needed.
  bool generic = types_->MentionsParameter(callee);
  if (generic) {
    TypeStore::Substitution substitution;
    std::vector<TypeId> found;
    found.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      // Checked without an expectation: the expectation is what is being
      // inferred. A literal takes its default type, because `T` has no
      // context to narrow it.
      // Checked with no expectation, which is also what settles a literal
      // on its default type -- `T` gives it no context to narrow to.
      const TypeId actual = Adapt(Check(args[i], TypeId{}), TypeId{},
                                  ast_.Get(args[i]).span);
      found.push_back(actual);
      if (i < params.size()) {
        types_->Unify(params[i], actual, &substitution);
      }
    }

    for (size_t i = 0; i < params.size() && i < found.size(); ++i) {
      const TypeId expected = types_->Substitute(params[i], substitution);
      if (!Accepts(expected, found[i])) {
        ReportMismatch(expected, found[i], ast_.Get(args[i]).span);
      }
    }
    if (params.size() != args.size()) {
      std::string message = "expected ";
      message.append(std::to_string(params.size()));
      message.append(params.size() == 1 ? " argument, found "
                                        : " arguments, found ");
      message.append(std::to_string(args.size()));
      Report(kWrongArguments, std::move(message), expr.span);
    }
    return types_->Substitute(result, substitution);
  }

  if (params.size() != args.size()) {
    std::string message = "expected ";
    message.append(std::to_string(params.size()));
    message.append(params.size() == 1 ? " argument, found " : " arguments, found ");
    message.append(std::to_string(args.size()));
    Report(kWrongArguments, std::move(message), expr.span);
  }

  for (size_t i = 0; i < args.size(); ++i) {
    const TypeId expected = i < params.size() ? params[i] : TypeId{};
    const TypeId found = Check(args[i], expected);
    if (expected.is_valid() && !Accepts(expected, found)) {
      ReportMismatch(expected, found, ast_.Get(args[i]).span);
    }
  }
  return result;
}

TypeId Checker::CheckIndex(const Expr& expr) {
  const TypeId container = Check(expr.lhs, TypeId{});
  Check(expr.rhs, TypeId{});

  const TypeKind kind = types_->KindOf(container);
  if (kind == TypeKind::kSlice || kind == TypeKind::kArray) {
    // Indexing with a range produces a slice of the same element type.
    if (ast_.Contains(expr.rhs) &&
        ast_.Get(expr.rhs).kind == ExprKind::kRange) {
      return types_->Slice(types_->ElementOf(container),
                           types_->Get(container).is_soa);
    }
    return types_->ElementOf(container);
  }
  if (!types_->IsError(container)) {
    std::string message = "`";
    message.append(Describe(container));
    message.append("` cannot be indexed");
    Report(kNotIndexable, std::move(message), expr.span);
  }
  return types_->error();
}

// `Vec3(x, y, z)` fills the type's fields in declaration order. A type with
// no fields known -- declared in a file this one cannot see -- still has its
// arguments checked, just without expectations.
void Checker::CheckConstructor(const Expr& expr, TypeId type) {
  const std::span<const ExprId> args = ast_.Get(expr.args);

  std::vector<TypeId> fields;
  for (const Member& member : resolution_->MembersOf(type)) {
    if (member.kind == MemberKind::kField) {
      fields.push_back(member.type);
    }
  }

  // No arguments is always allowed: it asks for the default value.
  if (!args.empty() && !fields.empty() && args.size() != fields.size()) {
    std::string message = "`";
    message.append(Describe(type));
    message.append("` has ");
    message.append(std::to_string(fields.size()));
    message.append(fields.size() == 1 ? " field, found " : " fields, found ");
    message.append(std::to_string(args.size()));
    message.append(args.size() == 1 ? " argument" : " arguments");
    Report(kWrongArguments, std::move(message), expr.span);
  }

  const bool positional = args.size() == fields.size();
  for (size_t i = 0; i < args.size(); ++i) {
    const TypeId expected = positional ? fields[i] : TypeId{};
    const TypeId found = Check(args[i], expected);
    if (expected.is_valid() && !Accepts(expected, found)) {
      ReportMismatch(expected, found, ast_.Get(args[i]).span);
    }
  }
}

TypeId Checker::CheckField(const Expr& expr) {
  const TypeId object = Check(expr.lhs, TypeId{});

  if (expr.kind == ExprKind::kField && types_->IsOptional(object)) {
    // Plain `.` on an optional is the bug nullability exists to catch; `?.`
    // is the form that handles absence.
    std::string message = "`";
    message.append(Describe(object));
    message.append("` may be absent");
    Report(kMaybeNull, std::move(message), expr.span,
           "use `?.` or check it with `if let` first");
    return types_->error();
  }

  // `?.` on an optional reaches through to the member of the inner type, and
  // the whole expression is optional again.
  const bool through_optional =
      expr.kind != ExprKind::kField && types_->IsOptional(object);
  const TypeId subject =
      through_optional ? types_->ElementOf(object) : object;
  const TypeId named = types_->StripWrappers(subject);

  if (types_->IsError(named)) {
    return types_->error();
  }

  const Member* member = resolution_->FindMember(named, expr.name);
  if (member == nullptr) {
    if (!resolution_->MembersOf(named).empty()) {
      std::string message = "`";
      message.append(Describe(named));
      message.append("` has no member named `");
      message.append(interner_->Text(expr.name));
      message.append("`");
      Report(kUnknownMember, std::move(message), expr.span);
    }
    // A type whose members are not known is declared in a file this one
    // cannot see. That is already reported as an unresolved import.
    return types_->error();
  }

  return through_optional ? types_->Optional(member->type) : member->type;
}

TypeId Checker::CheckIf(const Expr& expr, TypeId expected) {
  PushScope();
  if (HasFlag(expr.flags, ExprFlags::kHasBinding)) {
    // `if let x = optional` narrows the optional for the body, which is the
    // whole point of flow typing.
    const TypeId subject = Check(expr.lhs, TypeId{});
    const TypeId narrowed = types_->IsOptional(subject)
                                ? types_->ElementOf(subject)
                                : subject;
    BindPattern(expr.pattern, narrowed);
  } else {
    const TypeId condition = Check(expr.lhs, types_->bool_type());
    if (!Accepts(types_->bool_type(), condition)) {
      ReportMismatch(types_->bool_type(), condition, expr.span);
    }
  }

  const TypeId then_type = Check(expr.rhs, expected);
  PopScope();

  if (!expr.extra.is_valid()) {
    // Without an `else` the value is only produced on one path, so the whole
    // expression is a statement.
    return types_->void_type();
  }
  const TypeId else_type =
      Check(expr.extra, expected.is_valid() ? expected : then_type);
  if (expected.is_valid()) {
    return expected;
  }
  if (types_->KindOf(then_type) == TypeKind::kNever) {
    return else_type;
  }
  if (types_->KindOf(else_type) == TypeKind::kNever) {
    return then_type;
  }
  if (then_type != else_type && !Accepts(then_type, else_type)) {
    ReportMismatch(then_type, else_type, expr.span);
  }
  return then_type;
}

// A `match` has to cover every value its subject can take, because a value
// that falls through has nowhere to go. Only bool, optionals and enums have a
// value set the compiler can enumerate; anything else needs a catch-all arm.
void Checker::CheckExhaustive(const Expr& expr, TypeId subject) {
  if (types_->IsError(subject)) {
    return;
  }

  // An enum's variants are its members. Gathered first, because a bare name
  // in an arm is the short form of a variant when the subject is that enum,
  // and an ordinary binding -- a catch-all -- when it is not.
  std::vector<Symbol> variants;
  for (const Member& member : resolution_->MembersOf(
           types_->StripWrappers(subject))) {
    if (member.kind == MemberKind::kVariant) {
      variants.push_back(member.name);
    }
  }
  const auto is_variant = [&variants](Symbol name) {
    for (Symbol variant : variants) {
      if (variant == name) {
        return true;
      }
    }
    return false;
  };

  bool has_catch_all = false;
  bool covers_true = false;
  bool covers_false = false;
  bool covers_null = false;
  bool covers_present = false;
  std::vector<Symbol> covered_variants;

  for (MatchArmId arm_id : ast_.Get(expr.arms)) {
    const MatchArm& arm = ast_.Get(arm_id);
    // A guarded arm may not run, so it covers nothing.
    if (arm.guard.is_valid()) {
      continue;
    }
    for (PatternId pattern_id : ast_.Get(arm.patterns)) {
      if (!ast_.Contains(pattern_id)) {
        continue;
      }
      const Pattern& pattern = ast_.Get(pattern_id);
      switch (pattern.kind) {
        case PatternKind::kBinding:
          if (is_variant(pattern.name)) {
            covered_variants.push_back(pattern.name);
          } else {
            has_catch_all = true;
          }
          break;
        case PatternKind::kWildcard:
        case PatternKind::kRest:
          has_catch_all = true;
          break;
        case PatternKind::kPresent:
          covers_present = true;
          break;
        case PatternKind::kLiteral: {
          if (!ast_.Contains(pattern.literal)) {
            break;
          }
          const Expr& literal = ast_.Get(pattern.literal);
          if (literal.kind == ExprKind::kBoolLiteral) {
            if (literal.aux != 0) {
              covers_true = true;
            } else {
              covers_false = true;
            }
          } else if (literal.kind == ExprKind::kNullLiteral) {
            covers_null = true;
          }
          break;
        }
        case PatternKind::kVariant:
        case PatternKind::kStruct: {
          // The variant name is the last segment of the pattern's path;
          // `name` holds the binding of a struct pattern's field instead.
          const std::span<const Symbol> path = ast_.Get(pattern.path);
          if (!path.empty()) {
            covered_variants.push_back(path.back());
          } else if (pattern.name.is_valid()) {
            covered_variants.push_back(pattern.name);
          }
          break;
        }
        default:
          break;
      }
    }
  }

  if (has_catch_all) {
    return;
  }

  const auto report = [&](std::string missing) {
    std::string message = "`match` does not cover every value of `";
    message.append(Describe(subject));
    message.append("`");
    Report(kNonExhaustive, std::move(message), expr.span,
           "add an arm for " + missing + ", or a `_` arm");
  };

  if (types_->KindOf(subject) == TypeKind::kBool) {
    if (!covers_true || !covers_false) {
      report(covers_true ? "`false`" : "`true`");
    }
    return;
  }

  if (types_->IsOptional(subject)) {
    if (!covers_null || !covers_present) {
      report(covers_null ? "`x?`" : "`null`");
    }
    return;
  }

  // The uncovered variants are known by name, so they can be listed.
  if (!variants.empty()) {
    std::string missing;
    for (Symbol variant : variants) {
      bool found = false;
      for (Symbol covered : covered_variants) {
        if (covered == variant) {
          found = true;
          break;
        }
      }
      if (found) {
        continue;
      }
      if (!missing.empty()) {
        missing.append(", ");
      }
      missing.push_back('`');
      missing.append(interner_->Text(variant));
      missing.push_back('`');
    }
    if (!missing.empty()) {
      report(std::move(missing));
    }
    return;
  }

  // Every other type has more values than a `match` can spell out, so the
  // only exhaustive form is one with a catch-all.
  report("every remaining value");
}

TypeId Checker::CheckMatch(const Expr& expr, TypeId expected) {
  const TypeId subject = Check(expr.lhs, TypeId{});
  TypeId result;

  for (MatchArmId arm_id : ast_.Get(expr.arms)) {
    const MatchArm& arm = ast_.Get(arm_id);
    PushScope();
    for (PatternId pattern : ast_.Get(arm.patterns)) {
      BindPattern(pattern, subject);
    }
    if (arm.guard.is_valid()) {
      const TypeId guard = Check(arm.guard, types_->bool_type());
      if (!Accepts(types_->bool_type(), guard)) {
        ReportMismatch(types_->bool_type(), guard, arm.span);
      }
    }
    const TypeId body = Check(arm.body, expected.is_valid() ? expected
                                                            : result);
    PopScope();

    if (!result.is_valid() ||
        types_->KindOf(result) == TypeKind::kNever) {
      result = body;
    } else if (types_->KindOf(body) != TypeKind::kNever &&
               !Accepts(result, body)) {
      ReportMismatch(result, body, arm.span);
    }
  }

  CheckExhaustive(expr, subject);

  if (expected.is_valid()) {
    return expected;
  }
  return result.is_valid() ? result : types_->void_type();
}

TypeId Checker::CheckLoop(const Expr& expr) {
  PushScope();
  ++loop_depth_;

  if (expr.kind == ExprKind::kWhile) {
    if (HasFlag(expr.flags, ExprFlags::kHasBinding)) {
      const TypeId subject = Check(expr.lhs, TypeId{});
      BindPattern(expr.pattern, types_->IsOptional(subject)
                                    ? types_->ElementOf(subject)
                                    : subject);
    } else {
      const TypeId condition = Check(expr.lhs, types_->bool_type());
      if (!Accepts(types_->bool_type(), condition)) {
        ReportMismatch(types_->bool_type(), condition, expr.span);
      }
    }
  } else if (expr.kind == ExprKind::kFor ||
             expr.kind == ExprKind::kForeach) {
    const TypeId sequence = Check(expr.lhs, TypeId{});
    // A slice or array yields its elements; a range yields its bound type.
    TypeId element = types_->ElementOf(sequence);
    if (!element.is_valid()) {
      element = types_->IsNumeric(sequence) ? sequence : types_->error();
    }
    BindPattern(expr.pattern, element);
  }

  Check(expr.rhs, TypeId{});
  --loop_depth_;
  PopScope();
  return types_->void_type();
}

TypeId Checker::CheckRange(const Expr& expr) {
  TypeId bound;
  if (expr.lhs.is_valid()) {
    bound = Check(expr.lhs, TypeId{});
  }
  if (expr.rhs.is_valid()) {
    const TypeId upper = Check(expr.rhs, bound);
    if (!bound.is_valid()) {
      bound = upper;
    } else if (!Accepts(bound, upper)) {
      ReportMismatch(bound, upper, expr.span);
    }
  }
  if (!bound.is_valid()) {
    bound = types_->default_int();
  }
  // A range is a sequence of its bound type, so `for i in 0..10` binds an
  // integer without a special case.
  return types_->Slice(Adapt(bound, TypeId{}, expr.span), false);
}

TypeId Checker::Check(ExprId id, TypeId expected, bool keep_open) {
  if (!ast_.Contains(id)) {
    return types_->error();
  }
  const Expr& expr = ast_.Get(id);
  TypeId type;

  switch (expr.kind) {
    case ExprKind::kError:
      type = types_->error();
      break;

    case ExprKind::kIntLiteral:
    case ExprKind::kFloatLiteral:
    case ExprKind::kStringLiteral:
    case ExprKind::kCharLiteral:
    case ExprKind::kBoolLiteral:
    case ExprKind::kNullLiteral:
      type = CheckLiteral(expr, expected);
      break;

    case ExprKind::kStringText:
      type = types_->str();
      break;

    case ExprKind::kPath:
      type = CheckPath(expr);
      break;

    case ExprKind::kSelf:
      type = self_type_.is_valid() ? self_type_ : types_->error();
      break;

    case ExprKind::kUnary:
      type = CheckUnary(expr, expected);
      break;
    case ExprKind::kBinary:
      type = CheckBinary(expr);
      break;
    case ExprKind::kAssign:
      type = CheckAssign(expr);
      break;
    case ExprKind::kRange:
      type = CheckRange(expr);
      break;

    case ExprKind::kCast:
      Check(expr.lhs, TypeId{});
      type = resolver_.Resolve(ast_, expr.type);
      break;

    case ExprKind::kCall:
      type = CheckCall(expr);
      break;
    case ExprKind::kIndex:
      type = CheckIndex(expr);
      break;
    case ExprKind::kField:
    case ExprKind::kOptionalField:
      type = CheckField(expr);
      break;

    case ExprKind::kTry: {
      const TypeId inner = Check(expr.lhs, TypeId{});
      // `?` unwraps one layer of fallibility or absence.
      type = types_->IsFallible(inner) || types_->IsOptional(inner)
                 ? types_->ElementOf(inner)
                 : inner;
      break;
    }

    case ExprKind::kAwait:
    case ExprKind::kSpawn:
      type = Check(expr.lhs, TypeId{});
      break;

    case ExprKind::kStructLiteral:
      Check(expr.lhs, TypeId{});
      for (ExprId field : ast_.Get(expr.args)) {
        Check(field, TypeId{});
      }
      type = types_->error();
      break;

    case ExprKind::kArrayLiteral: {
      const std::span<const ExprId> elements = ast_.Get(expr.args);
      // An expected array or slice type tells the elements what to be.
      TypeId element_expectation;
      const TypeKind expected_kind = types_->KindOf(expected);
      if (expected_kind == TypeKind::kArray ||
          expected_kind == TypeKind::kSlice) {
        element_expectation = types_->ElementOf(expected);
      }

      TypeId element = element_expectation;
      for (ExprId item : elements) {
        const TypeId found = Check(item, element_expectation.is_valid()
                                             ? element_expectation
                                             : element);
        if (!element.is_valid() || types_->IsLiteral(element)) {
          element = found;
        } else if (!Accepts(element, found)) {
          ReportMismatch(element, found, ast_.Get(item).span);
        }
      }
      if (!element.is_valid()) {
        element = types_->error();
      }
      element = Adapt(element, element_expectation, expr.span);
      type = types_->Array(element, elements.size(), false);
      break;
    }

    case ExprKind::kTupleLiteral: {
      std::vector<TypeId> elements;
      const std::vector<TypeId> expectations =
          types_->KindOf(expected) == TypeKind::kTuple
              ? types_->ArgumentsOf(expected)
              : std::vector<TypeId>();
      const std::span<const ExprId> items = ast_.Get(expr.args);
      for (size_t i = 0; i < items.size(); ++i) {
        const TypeId expectation =
            i < expectations.size() ? expectations[i] : TypeId{};
        elements.push_back(
            Adapt(Check(items[i], expectation), expectation,
                  ast_.Get(items[i]).span));
      }
      type = types_->Tuple(elements);
      break;
    }

    case ExprKind::kClosure: {
      PushScope();
      std::vector<TypeId> params;
      for (ParamId param_id : ast_.Get(expr.params)) {
        const Param& param = ast_.Get(param_id);
        const TypeId param_type = param.type.is_valid()
                                      ? resolver_.Resolve(ast_, param.type)
                                      : types_->error();
        params.push_back(param_type);
        Bind(param.name, param_type, param.is_mutable);
      }
      const TypeId declared = expr.type.is_valid()
                                  ? resolver_.Resolve(ast_, expr.type)
                                  : TypeId{};
      const TypeId body = Check(expr.lhs, declared);
      PopScope();
      type = types_->Function(params,
                              declared.is_valid() ? declared : body);
      break;
    }

    case ExprKind::kBlock:
      CheckBlock(id, expected);
      return result_.TypeOf(id);

    case ExprKind::kIf:
      type = CheckIf(expr, expected);
      break;
    case ExprKind::kMatch:
      type = CheckMatch(expr, expected);
      break;
    case ExprKind::kLoop:
    case ExprKind::kWhile:
    case ExprKind::kFor:
    case ExprKind::kForeach:
      type = CheckLoop(expr);
      break;

    case ExprKind::kUnsafe:
    case ExprKind::kRegion:
      type = Check(expr.lhs, expected);
      break;
  }

  if (!(keep_open && !expected.is_valid())) {
    type = Adapt(type, expected, expr.span);
  }
  Record(id, type);
  return type;
}

// -------------------------------------------------------------- patterns ----

void Checker::BindPattern(PatternId id, TypeId type,
                          bool is_mutable) {
  if (!ast_.Contains(id)) {
    return;
  }
  const Pattern& pattern = ast_.Get(id);
  switch (pattern.kind) {
    case PatternKind::kBinding: {
      // A bare name that is a variant of the subject's enum is the short
      // form of that variant, not a new binding that would shadow it.
      const Member* member = resolution_->FindMember(
          types_->StripWrappers(type), pattern.name);
      if (member != nullptr && member->kind == MemberKind::kVariant) {
        return;
      }
      Bind(pattern.name, type,
           is_mutable || HasFlag(pattern.flags, PatternFlags::kMutable));
      return;
    }

    case PatternKind::kPresent: {
      // `n?` matches a present optional and binds what is inside.
      const TypeId inner = types_->IsOptional(type)
                               ? types_->ElementOf(type)
                               : type;
      for (PatternId element : ast_.Get(pattern.elements)) {
        BindPattern(element, inner, is_mutable);
      }
      return;
    }

    case PatternKind::kTuple: {
      const std::vector<TypeId> elements =
          types_->KindOf(type) == TypeKind::kTuple
              ? types_->ArgumentsOf(type)
              : std::vector<TypeId>();
      const std::span<const PatternId> patterns = ast_.Get(pattern.elements);
      for (size_t i = 0; i < patterns.size(); ++i) {
        BindPattern(patterns[i],
                    i < elements.size() ? elements[i] : types_->error(),
                    is_mutable);
      }
      return;
    }

    case PatternKind::kVariant:
    case PatternKind::kStruct:
      // Variant payloads and field types arrive with member resolution.
      for (PatternId element : ast_.Get(pattern.elements)) {
        BindPattern(element, types_->error());
      }
      return;

    case PatternKind::kLiteral: {
      if (!pattern.literal.is_valid()) {
        return;
      }
      // `null` matches the optional itself; every other literal matches what
      // is inside it, so `match count { 0 => ... }` on a `u8?` still catches
      // a literal that does not fit.
      const bool matches_absence =
          ast_.Contains(pattern.literal) &&
          ast_.Get(pattern.literal).kind == ExprKind::kNullLiteral;
      Check(pattern.literal,
            matches_absence ? type : types_->StripWrappers(type));
      return;
    }

    case PatternKind::kWildcard:
    case PatternKind::kRest:
    case PatternKind::kError:
      return;
  }
}

// ------------------------------------------------------------ statements ----

void Checker::CheckStmt(StmtId id) {
  if (!ast_.Contains(id)) {
    return;
  }
  const Stmt& stmt = ast_.Get(id);

  switch (stmt.kind) {
    case StmtKind::kLet:
    case StmtKind::kVar: {
      const TypeId declared = stmt.type.is_valid()
                                  ? resolver_.Resolve(ast_, stmt.type)
                                  : TypeId{};
      TypeId value = declared;
      if (stmt.expr.is_valid()) {
        value = Check(stmt.expr, declared);
        if (declared.is_valid() && !Accepts(declared, value)) {
          ReportMismatch(declared, value, ast_.Get(stmt.expr).span);
          value = declared;
        }
      }
      // Without an annotation and without an initializer there is nothing to
      // infer from; the binding gets the error type and says so once.
      if (!value.is_valid()) {
        value = types_->error();
      }
      BindPattern(stmt.pattern, Adapt(value, declared, stmt.span),
                  stmt.kind == StmtKind::kVar);
      return;
    }

    case StmtKind::kExpr:
      Check(stmt.expr, TypeId{});
      return;

    case StmtKind::kReturn: {
      if (!stmt.expr.is_valid()) {
        return;
      }
      const TypeId found = Check(stmt.expr, return_type_);
      if (return_type_.is_valid() && !Accepts(return_type_, found)) {
        ReportMismatch(return_type_, found, ast_.Get(stmt.expr).span);
      }
      return;
    }

    case StmtKind::kBreak:
    case StmtKind::kContinue:
      return;

    case StmtKind::kDefer:
      Check(stmt.expr, TypeId{});
      return;

    case StmtKind::kItem:
      CheckItem(stmt.item);
      return;

    case StmtKind::kError:
      return;
  }
}

void Checker::CheckBlock(ExprId id, TypeId expected) {
  const Expr& block = ast_.Get(id);
  PushScope();

  const std::span<const StmtId> stmts = ast_.Get(block.stmts);
  TypeId type = types_->void_type();
  for (size_t i = 0; i < stmts.size(); ++i) {
    const bool is_last = i + 1 == stmts.size();
    const Stmt& stmt = ast_.Get(stmts[i]);
    // A block's value is its final expression statement, which is what makes
    // `let x = { ... }` and an expression-bodied arm work.
    if (is_last && stmt.kind == StmtKind::kExpr) {
      type = Check(stmt.expr, expected);
      continue;
    }
    CheckStmt(stmts[i]);
  }

  PopScope();
  Record(id, type);
}

// ---------------------------------------------------------- declarations ----

void Checker::CheckFunction(const Item& item) {
  PushScope();

  size_t pushed = 0;
  for (GenericParamId generic_id : ast_.Get(item.generics)) {
    const GenericParam& generic = ast_.Get(generic_id);
    const TypeId parameter =
        types_->Parameter(generic.name, 0, static_cast<uint32_t>(pushed));
    resolver_.PushTypeParameter(generic.name, parameter);
    ++pushed;
  }

  for (ParamId param_id : ast_.Get(item.params)) {
    const Param& param = ast_.Get(param_id);
    const TypeId type = param.type.is_valid()
                            ? resolver_.Resolve(ast_, param.type)
                            : types_->error();
    if (param.default_value.is_valid()) {
      const TypeId found = Check(param.default_value, type);
      if (!Accepts(type, found)) {
        ReportMismatch(type, found, ast_.Get(param.default_value).span);
      }
    }
    Bind(param.name, type, param.is_mutable);
  }

  const TypeId previous_return = return_type_;
  return_type_ = item.type.is_valid() ? resolver_.Resolve(ast_, item.type)
                                      : types_->void_type();

  if (item.body.is_valid()) {
    if (HasFlag(item.flags, ItemFlags::kExpressionBody)) {
      // `fn f() -> T => expr` checks the expression against the return type,
      // with no `return` statement in sight.
      const TypeId found = Check(item.body, return_type_);
      if (!Accepts(return_type_, found)) {
        ReportMismatch(return_type_, found, ast_.Get(item.body).span);
      }
    } else {
      Check(item.body, TypeId{});
    }
  }

  return_type_ = previous_return;
  resolver_.PopTypeParameters(pushed);
  PopScope();
}

void Checker::CheckBinding(const Item& item) {
  const TypeId declared = item.type.is_valid()
                              ? resolver_.Resolve(ast_, item.type)
                              : TypeId{};
  if (!item.init.is_valid()) {
    return;
  }
  const TypeId found = Check(item.init, declared);
  if (declared.is_valid() && !Accepts(declared, found)) {
    ReportMismatch(declared, found, ast_.Get(item.init).span);
  }
}

// `extend T as Trait` promises that T has every method the trait declares.
// The methods may come from T's own declaration or from any extension of it,
// which is why this works from the member table rather than from this block.
void Checker::CheckTraitConformance(const Item& item) {
  if (!item.super_type.is_valid() || !item.target_type.is_valid()) {
    return;
  }
  const TypeId trait = resolver_.Resolve(ast_, item.super_type);
  const TypeId target = resolver_.Resolve(ast_, item.target_type);
  if (types_->IsError(trait) || types_->IsError(target)) {
    return;
  }

  std::string missing;
  for (const Member& required : resolution_->MembersOf(trait)) {
    if (required.kind != MemberKind::kMethod) {
      continue;
    }
    const Member* found = resolution_->FindMember(target, required.name);
    if (found != nullptr && found->kind == MemberKind::kMethod) {
      continue;
    }
    if (!missing.empty()) {
      missing.append(", ");
    }
    missing.push_back('`');
    missing.append(interner_->Text(required.name));
    missing.push_back('`');
  }
  if (missing.empty()) {
    return;
  }

  std::string message = "`";
  message.append(Describe(target));
  message.append("` does not implement ");
  message.append(missing);
  message.append(" from `");
  message.append(Describe(trait));
  message.append("`");
  Report(kMissingTraitMethod, std::move(message), item.name_span);
}

void Checker::CheckItem(ItemId id) {
  if (!ast_.Contains(id)) {
    return;
  }
  const Item& item = ast_.Get(id);

  switch (item.kind) {
    case ItemKind::kFunction:
    case ItemKind::kSystem:
    case ItemKind::kOn:
      CheckFunction(item);
      return;

    case ItemKind::kBinding:
      CheckBinding(item);
      return;

    case ItemKind::kClass:
    case ItemKind::kStruct:
    case ItemKind::kTrait:
    case ItemKind::kComponent:
    case ItemKind::kEnum:
    case ItemKind::kExtend: {
      // Inside a type's members, `self` is that type.
      const TypeId previous_self = self_type_;
      if (item.kind == ItemKind::kExtend) {
        self_type_ = item.target_type.is_valid()
                         ? resolver_.Resolve(ast_, item.target_type)
                         : TypeId{};
        CheckTraitConformance(item);
      } else {
        const Binding* binding = Lookup(item.name);
        self_type_ = binding != nullptr ? binding->type : TypeId{};
      }
      for (ItemId member : ast_.Get(item.members)) {
        CheckItem(member);
      }
      self_type_ = previous_self;
      return;
    }

    case ItemKind::kUnit:
      // Resolved while building the file's scope, where the dimensions are.
      return;

    default:
      return;
  }
}

TypedBody Checker::Run(ItemId item) {
  PushScope();
  CheckItem(item);
  PopScope();
  result_.diagnostics = diagnostics_.diagnostics();
  return std::move(result_);
}

}  // namespace

TypedBody TypeCheckItem(TypeStore* types,
                        const StringInterner* interner,
                        const FileResolution* resolution,
                        const Ast& ast,
                        ItemId item) {
  Checker checker(types, interner, resolution, ast);
  return checker.Run(item);
}

}  // namespace wolang
