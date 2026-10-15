// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_SYNTAX_AST_H_
#define WOLANG_SYNTAX_AST_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/token.h"

namespace wolang {

// The syntax tree is stored as flat arrays of plain-data nodes, one array per
// category, with 32-bit handles instead of pointers. Child lists are ranges
// into per-category index pools. This keeps a whole file's tree contiguous, so
// it is cheap to build, cheap to walk, and serializable as-is for the
// incremental query cache in wave 3.

template <typename Tag>
struct AstId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }

  friend bool operator==(AstId lhs, AstId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(AstId lhs, AstId rhs) { return !(lhs == rhs); }
};

struct ItemTag;
struct ExprTag;
struct StmtTag;
struct TypeRefTag;
struct PatternTag;
struct AttributeTag;
struct ParamTag;
struct GenericParamTag;
struct MatchArmTag;
struct StateTag;
struct TransitionTag;
struct OrderingTag;

using ItemId = AstId<ItemTag>;
using ExprId = AstId<ExprTag>;
using StmtId = AstId<StmtTag>;
using TypeRefId = AstId<TypeRefTag>;
using PatternId = AstId<PatternTag>;
using AttributeId = AstId<AttributeTag>;
using ParamId = AstId<ParamTag>;
using GenericParamId = AstId<GenericParamTag>;
using MatchArmId = AstId<MatchArmTag>;
using StateId = AstId<StateTag>;
using TransitionId = AstId<TransitionTag>;
using OrderingId = AstId<OrderingTag>;

// A contiguous child list inside the pool for |Id|.
template <typename Id>
struct AstRange {
  uint32_t begin = 0;
  uint32_t count = 0;

  bool empty() const { return count == 0; }
  size_t size() const { return count; }
};

// ---------------------------------------------------------------- kinds -----

enum class ItemKind : uint8_t {
  kError,
  kNeed,       // `need a.b` -- an import
  kFunction,
  kClass,
  kStruct,
  kEnum,
  kVariant,   // A member of an enum.
  kAbility,
  kExtend,
  kComponent,
  kTag,
  kSystem,
  kSignal,
  kOn,        // `on Signal(args) { ... }`
  kBinding,   // `let`, `var` and `derived var` at declaration level.
  kTypeAlias,
  kDimension,
  kUnit,
  kEffect,
  kErrorSet,  // `error NotFound, Corrupt`
  kMachine,
  // `deinit { ... }`: what a type does when a value of it goes away. A
  // declaration rather than a method with a special name, so that it
  // cannot be called, overridden or named by mistake.
  kDeinit,
};

enum class ItemFlags : uint16_t {
  kNone = 0,
  // Visible to other modules: the `offer` keyword.
  kOffer = 1 << 0,
  kPriv = 1 << 1,
  kAsync = 1 << 2,
  kVirtual = 1 << 3,
  kOverride = 1 << 4,
  kComptime = 1 << 5,
  kDerived = 1 << 6,
  // A `var` binding rather than a `let` binding.
  kMutable = 1 << 7,
  kExtern = 1 << 8,
  // The body was written as `=> expr` rather than as a block.
  kExpressionBody = 1 << 9,
  // The parser recovered inside this item; its children are incomplete.
  kHasError = 1 << 10,
  // `extend T as Ability` rather than `extend T`.
  kAbilityImpl = 1 << 11,
  // `need a.b.{c, d}` rather than `need a.b`.
  kNeedList = 1 << 12,
  // The declaration has a body. Recorded as a flag because the item tree
  // crosses files and the body does not: another module has to be able to
  // tell a `task` it may `wait` on -- one with no body, which suspends once
  // -- from one whose machine lives somewhere it cannot see.
  kHasBody = 1 << 13,
};

enum class ExprKind : uint8_t {
  kError,
  kIntLiteral,
  kFloatLiteral,
  kStringLiteral,   // Parts in `args`; see kStringText.
  kStringText,      // A literal chunk of a string; text in `aux`.
  kCharLiteral,
  kBoolLiteral,
  kNullLiteral,
  // `ptrnull`: the address no object has, for a binding that has to
  // pass or test one (D156).
  kPointerNull,
  kPath,
  kSelf,
  kUnary,
  kBinary,
  kAssign,
  kRange,
  kCast,
  kCall,
  kIndex,
  kField,           // `expr.name`
  kOptionalField,   // `expr?.name`
  kTry,             // `expr?`
  kAwait,
  kSpawn,
  kStructLiteral,   // `Path { field = value }`
  kArrayLiteral,
  kTupleLiteral,
  kClosure,
  kBlock,
  kIf,
  kMatch,
  kLoop,
  kWhile,
  kFor,
  kForeach,
  kUnsafe,
  kRegion,
};

enum class ExprFlags : uint16_t {
  kNone = 0,
  kHasError = 1 << 0,
  // `if let` / `while let` rather than a plain condition.
  kHasBinding = 1 << 1,
  // `&mut expr` or `*mut T` style mutability.
  kMutable = 1 << 2,
  // `..=` rather than `..`.
  kInclusive = 1 << 3,
  // The call carried explicit generic arguments.
  kHasGenericArgs = 1 << 4,
};

enum class StmtKind : uint8_t {
  kError,
  kLet,
  kVar,
  kExpr,
  kReturn,
  kBreak,
  kContinue,
  kDefer,
  kItem,
};

enum class StmtFlags : uint16_t {
  kNone = 0,
  kHasError = 1 << 0,
};

enum class TypeRefKind : uint8_t {
  kError,
  kPath,
  kOptional,   // `T?` -- may be absent
  kFallible,   // `T!` -- may fail
  kFunction,   // `fn(A, B) -> C`
  kPointer,    // `*T`, `*mut T`
  kReference,  // `&T`, `&mut T`
  kTuple,
  kSlice,      // `[T]`
  kArray,      // `[T; N]`
  kMut,        // `mut T` inside a query or parameter list
  kWith,       // `with Tag` inside a query
  kWithout,    // `without Tag` inside a query
};

enum class TypeRefFlags : uint16_t {
  kNone = 0,
  kHasError = 1 << 0,
  kMutable = 1 << 1,
  // Storage layout: one array per field rather than one array of records.
  // A property of the storage, not of the element type, so the same struct
  // can be stored both ways and the choice can be measured.
  kSoa = 1 << 2,
  kAos = 1 << 3,
};

enum class PatternKind : uint8_t {
  kError,
  kWildcard,  // `_`
  kBinding,   // `name`, `mut name`
  kLiteral,
  kTuple,
  kVariant,   // `Path(sub, sub)`
  kStruct,    // `Path { field: sub }`
  kRest,      // `..`
  // `x?` -- present, binding the contained value. The counterpart is the
  // `null` literal pattern, so optionals need no Some/None vocabulary.
  kPresent,
};

enum class PatternFlags : uint16_t {
  kNone = 0,
  kHasError = 1 << 0,
  kMutable = 1 << 1,
};

std::string_view ItemKindName(ItemKind kind);
std::string_view ExprKindName(ExprKind kind);
std::string_view StmtKindName(StmtKind kind);
std::string_view TypeRefKindName(TypeRefKind kind);
std::string_view PatternKindName(PatternKind kind);

// Flag helpers. The enums are scoped, so the operators are explicit.
#define WOLANG_AST_FLAG_OPS(Flags)                                      \
  inline Flags operator|(Flags lhs, Flags rhs) {                        \
    return static_cast<Flags>(static_cast<uint16_t>(lhs) |              \
                              static_cast<uint16_t>(rhs));              \
  }                                                                     \
  inline Flags& operator|=(Flags& lhs, Flags rhs) {                     \
    lhs = lhs | rhs;                                                    \
    return lhs;                                                         \
  }                                                                     \
  inline bool HasFlag(Flags flags, Flags flag) {                        \
    return (static_cast<uint16_t>(flags) & static_cast<uint16_t>(flag)) \
           != 0;                                                        \
  }

WOLANG_AST_FLAG_OPS(ItemFlags)
WOLANG_AST_FLAG_OPS(ExprFlags)
WOLANG_AST_FLAG_OPS(StmtFlags)
WOLANG_AST_FLAG_OPS(TypeRefFlags)
WOLANG_AST_FLAG_OPS(PatternFlags)

#undef WOLANG_AST_FLAG_OPS

// ---------------------------------------------------------------- nodes -----

// Declarations, including the members of a class, ability or enum. One struct
// covers every declaration kind: items are the least numerous nodes, and a
// uniform shape keeps the parser and every later pass free of casts.
struct Item {
  ItemKind kind = ItemKind::kError;
  ItemFlags flags = ItemFlags::kNone;
  Symbol name;
  // Covers the whole declaration, attributes included.
  SourceSpan span;
  // Covers just the declared name, which is where a diagnostic about the
  // declaration itself belongs.
  SourceSpan name_span;
  // The `///` lines attached to this declaration, or an invalid span.
  SourceSpan doc;

  AstRange<AttributeId> attributes;
  AstRange<GenericParamId> generics;
  // Function parameters, signal and `on` parameters, enum variant fields,
  // component and struct fields are members instead.
  AstRange<ParamId> params;
  // Class, struct, ability, impl, component and enum members.
  AstRange<ItemId> members;
  AstRange<StateId> states;
  AstRange<OrderingId> orderings;
  // The effect list written after `uses`.
  AstRange<TypeRefId> effects;
  // Import path, or the dimension a unit belongs to.
  AstRange<Symbol> path;
  // Names inside `import a.b.{c, d}`, or a `dimension A, B` list.
  AstRange<Symbol> names;

  // Binding and alias type, or a function's return type.
  TypeRefId type;
  // Class base type, or the ability of an ability impl.
  TypeRefId super_type;
  // The type an `impl` block applies to.
  TypeRefId target_type;

  // Function, system and `on` body.
  ExprId body;
  // Binding initializer, derived expression, or a unit's conversion factor.
  ExprId init;

  // `extern "C"` ABI name and the linker symbol of an external function.
  Symbol abi;
  Symbol link_name;
};

struct Expr {
  ExprKind kind = ExprKind::kError;
  ExprFlags flags = ExprFlags::kNone;
  // The operator of a unary, binary, assignment or compound assignment.
  TokenKind op = TokenKind::kUnknown;
  // Field name, or the single-identifier head of a struct literal.
  Symbol name;
  SourceSpan span;

  // Unary and `await`: operand in `lhs`. Binary and assignment: `lhs`, `rhs`.
  // Call and index: callee or container in `lhs`. If: condition in `lhs`, then
  // branch in `rhs`, else branch in `extra`. Loops: subject in `lhs`, body in
  // `rhs`. Match: scrutinee in `lhs`.
  ExprId lhs;
  ExprId rhs;
  ExprId extra;

  // Call arguments, array and tuple elements, string literal parts, struct
  // literal field assignments.
  AstRange<ExprId> args;
  AstRange<StmtId> stmts;
  AstRange<MatchArmId> arms;
  AstRange<TypeRefId> generic_args;
  AstRange<ParamId> params;

  // The binding of `if let`, `for`, `foreach` and `while let`.
  PatternId pattern;
  // The label of a loop, or the target of `break`/`continue`.
  Symbol label;
  // The target type of a cast.
  TypeRefId type;

  // Integer and float literals: index into Ast::numbers(). String text: index
  // into Ast::strings(). Character literals: the code point. Boolean literals:
  // zero or one.
  uint32_t aux = 0;
};

struct Stmt {
  StmtKind kind = StmtKind::kError;
  StmtFlags flags = StmtFlags::kNone;
  SourceSpan span;
  // The loop named by `break outer` or `continue outer`.
  Symbol label;
  // `let` and `var` binding.
  PatternId pattern;
  TypeRefId type;
  // Initializer, expression statement, returned value, deferred expression.
  ExprId expr;
  // The declaration of a nested item statement.
  ItemId item;
};

struct TypeRef {
  TypeRefKind kind = TypeRefKind::kError;
  TypeRefFlags flags = TypeRefFlags::kNone;
  SourceSpan span;
  AstRange<Symbol> path;
  // Generic arguments, tuple elements, or function parameter types.
  AstRange<TypeRefId> args;
  // The inner type of an optional, fallible, pointer, reference, slice,
  // array, `mut`, `with` or `without`, or the return type of a function type.
  TypeRefId element;
  // The length of `[T; N]`.
  ExprId length;
};

struct Pattern {
  PatternKind kind = PatternKind::kError;
  PatternFlags flags = PatternFlags::kNone;
  SourceSpan span;
  // The name this pattern binds.
  Symbol name;
  // When this pattern is a field of a struct pattern, the field it matches.
  // Separate from |name| because `Point { y: inner }` binds `inner` while
  // matching `y`, and conflating the two loses one of them.
  Symbol field;
  AstRange<Symbol> path;
  AstRange<PatternId> elements;
  ExprId literal;
};

struct Attribute {
  Symbol name;
  SourceSpan span;
  // Positional arguments, plus named arguments as assignment expressions.
  AstRange<ExprId> args;
};

struct Param {
  Symbol name;
  SourceSpan span;
  TypeRefId type;
  ExprId default_value;
  AstRange<AttributeId> attributes;
  bool is_mutable = false;
};

struct GenericParam {
  Symbol name;
  SourceSpan span;
  // Bounds from both `<T: Bound>` and a `where` clause.
  AstRange<TypeRefId> bounds;
};

struct MatchArm {
  // Alternatives separated by `|`.
  AstRange<PatternId> patterns;
  ExprId guard;
  ExprId body;
  SourceSpan span;
};

struct State {
  Symbol name;
  SourceSpan span;
  ExprId enter;
  ExprId exit;
  AstRange<TransitionId> transitions;
};

struct Transition {
  // The signal that triggers the transition; unset for `always`.
  Symbol event;
  Symbol target;
  // The delay of `after <expr>`.
  ExprId delay;
  SourceSpan span;
  bool is_always = false;
};

struct Ordering {
  AstRange<Symbol> path;
  SourceSpan span;
  bool is_after = true;
};

// ------------------------------------------------------------------ ast -----

namespace internal {

template <typename T, typename Id>
class AstTable {
 public:
  Id Add(T node) {
    const auto index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(std::move(node));
    return Id{index};
  }

  const T& Get(Id id) const { return nodes_[id.index]; }
  T& GetMutable(Id id) { return nodes_[id.index]; }
  bool Contains(Id id) const {
    return id.is_valid() && id.index < nodes_.size();
  }
  size_t size() const { return nodes_.size(); }

 private:
  std::vector<T> nodes_;
};

template <typename Id>
class AstPool {
 public:
  AstRange<Id> Add(std::span<const Id> ids) {
    const auto begin = static_cast<uint32_t>(ids_.size());
    ids_.insert(ids_.end(), ids.begin(), ids.end());
    return AstRange<Id>{begin, static_cast<uint32_t>(ids.size())};
  }

  std::span<const Id> Get(AstRange<Id> range) const {
    if (range.count == 0 || range.begin >= ids_.size()) {
      return {};
    }
    return std::span<const Id>(ids_.data() + range.begin, range.count);
  }

  size_t size() const { return ids_.size(); }

 private:
  std::vector<Id> ids_;
};

}  // namespace internal

// The syntax tree of one file.
//
// Every accessor is an overload of Get(), so walking the tree reads the same
// no matter which category a handle belongs to:
//
//   for (ItemId id : ast.Get(ast.root())) {
//     const Item& item = ast.Get(id);
//   }
class Ast {
 public:
  Ast() = default;
  explicit Ast(FileId file) : file_(file) {}

  Ast(const Ast&) = delete;
  Ast& operator=(const Ast&) = delete;
  Ast(Ast&&) = default;
  Ast& operator=(Ast&&) = default;

  FileId file() const { return file_; }
  void set_file(FileId file) { file_ = file; }

  // The top-level declarations of the file, in source order.
  AstRange<ItemId> root() const { return root_; }
  void set_root(AstRange<ItemId> root) { root_ = root; }

  // The file-level attributes written before any declaration.
  AstRange<AttributeId> file_attributes() const { return file_attributes_; }
  void set_file_attributes(AstRange<AttributeId> attributes) {
    file_attributes_ = attributes;
  }

#define WOLANG_AST_CATEGORY(Type, Id, member)                       \
  Id Add(Type node) { return member##s_.Add(std::move(node)); }     \
  const Type& Get(Id id) const { return member##s_.Get(id); }      \
  Type& GetMutable(Id id) { return member##s_.GetMutable(id); }     \
  bool Contains(Id id) const { return member##s_.Contains(id); }    \
  size_t member##_count() const { return member##s_.size(); }       \
  AstRange<Id> AddRange(std::span<const Id> ids) {                  \
    return member##_pool_.Add(ids);                                 \
  }                                                                 \
  std::span<const Id> Get(AstRange<Id> range) const {               \
    return member##_pool_.Get(range);                               \
  }

  WOLANG_AST_CATEGORY(Item, ItemId, item)
  WOLANG_AST_CATEGORY(Expr, ExprId, expr)
  WOLANG_AST_CATEGORY(Stmt, StmtId, stmt)
  WOLANG_AST_CATEGORY(TypeRef, TypeRefId, type_ref)
  WOLANG_AST_CATEGORY(Pattern, PatternId, pattern)
  WOLANG_AST_CATEGORY(Attribute, AttributeId, attribute)
  WOLANG_AST_CATEGORY(Param, ParamId, param)
  WOLANG_AST_CATEGORY(GenericParam, GenericParamId, generic_param)
  WOLANG_AST_CATEGORY(MatchArm, MatchArmId, match_arm)
  WOLANG_AST_CATEGORY(State, StateId, state)
  WOLANG_AST_CATEGORY(Transition, TransitionId, transition)
  WOLANG_AST_CATEGORY(Ordering, OrderingId, ordering)

#undef WOLANG_AST_CATEGORY

  // Paths and name lists are stored as symbol ranges.
  AstRange<Symbol> AddRange(std::span<const Symbol> symbols) {
    return symbol_pool_.Add(symbols);
  }
  std::span<const Symbol> Get(AstRange<Symbol> range) const {
    return symbol_pool_.Get(range);
  }

  // Decoded numeric literals, referenced by Expr::aux.
  uint32_t AddNumber(NumberLiteral number) {
    numbers_.push_back(number);
    return static_cast<uint32_t>(numbers_.size() - 1);
  }
  const NumberLiteral& number(uint32_t index) const {
    return numbers_[index];
  }
  size_t number_count() const { return numbers_.size(); }

  // Decoded string contents, referenced by Expr::aux of kStringText nodes.
  uint32_t AddString(std::string text) {
    strings_.push_back(std::move(text));
    return static_cast<uint32_t>(strings_.size() - 1);
  }
  const std::string& string(uint32_t index) const { return strings_[index]; }
  size_t string_count() const { return strings_.size(); }

  size_t NodeCount() const {
    return items_.size() + exprs_.size() + stmts_.size() +
           type_refs_.size() + patterns_.size();
  }

 private:
  FileId file_;
  AstRange<ItemId> root_;
  AstRange<AttributeId> file_attributes_;

  internal::AstTable<Item, ItemId> items_;
  internal::AstTable<Expr, ExprId> exprs_;
  internal::AstTable<Stmt, StmtId> stmts_;
  internal::AstTable<TypeRef, TypeRefId> type_refs_;
  internal::AstTable<Pattern, PatternId> patterns_;
  internal::AstTable<Attribute, AttributeId> attributes_;
  internal::AstTable<Param, ParamId> params_;
  internal::AstTable<GenericParam, GenericParamId> generic_params_;
  internal::AstTable<MatchArm, MatchArmId> match_arms_;
  internal::AstTable<State, StateId> states_;
  internal::AstTable<Transition, TransitionId> transitions_;
  internal::AstTable<Ordering, OrderingId> orderings_;

  internal::AstPool<ItemId> item_pool_;
  internal::AstPool<ExprId> expr_pool_;
  internal::AstPool<StmtId> stmt_pool_;
  internal::AstPool<TypeRefId> type_ref_pool_;
  internal::AstPool<PatternId> pattern_pool_;
  internal::AstPool<AttributeId> attribute_pool_;
  internal::AstPool<ParamId> param_pool_;
  internal::AstPool<GenericParamId> generic_param_pool_;
  internal::AstPool<MatchArmId> match_arm_pool_;
  internal::AstPool<StateId> state_pool_;
  internal::AstPool<TransitionId> transition_pool_;
  internal::AstPool<OrderingId> ordering_pool_;
  internal::AstPool<Symbol> symbol_pool_;

  std::vector<NumberLiteral> numbers_;
  std::vector<std::string> strings_;
};

}  // namespace wolang

#endif  // WOLANG_SYNTAX_AST_H_
