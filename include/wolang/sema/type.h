// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_SEMA_TYPE_H_
#define WOLANG_SEMA_TYPE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/sema/dimension.h"
#include "wolang/sema/effect.h"

namespace wolang {

// A type is a 32-bit handle into a TypeStore. Types are interned, so two
// handles are equal exactly when the types are, and type equality costs one
// integer comparison everywhere in the compiler.
struct TypeId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }

  friend bool operator==(TypeId lhs, TypeId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(TypeId lhs, TypeId rhs) { return !(lhs == rhs); }
};

enum class TypeKind : uint8_t {
  // A type that could not be determined. Propagates silently so one real
  // error does not cascade into a dozen follow-ups.
  kError,
  // The absence of a value, as in a function that returns nothing.
  kVoid,
  // The type of an expression that never produces a value: `return`, `break`,
  // a call to a diverging function. Converts to anything.
  kNever,
  kBool,
  kChar,
  kInt,
  kFloat,
  // An integer literal before its type is known: arbitrary precision, and it
  // takes the type its context expects.
  kIntLiteral,
  // A float literal before its type is known.
  kFloatLiteral,
  kString,
  kStr,
  // A declared struct, class, enum, ability, component or tag.
  kNamed,
  // A type parameter inside a generic declaration.
  kParameter,
  // A named type applied to arguments: `Array<int>`.
  kInstance,
  kOptional,   // `T?`
  kFallible,   // `T!`
  kSlice,      // `[T]`
  kArray,      // `[T; N]`
  kTuple,
  kFunction,
  kPointer,
  kReference,
  // A number carrying a dimension: the type of `10m` and of `10m / 2s`.
  kQuantity,
  // The set of errors a `T!` may carry.
  kErrorSet,
  // `C.Item`: the associated type `Item` of whatever `C` turns out to be.
  // The owner is `base` and the name is `name`; it becomes a concrete type
  // once `C` does (D212).
  kProjection,
};

// Width and signedness of an integer type.
enum class IntWidth : uint8_t { k8, k16, k32, k64 };
enum class FloatWidth : uint8_t { k32, k64 };

std::string_view TypeKindName(TypeKind kind);

// One interned type. Plain data with no owned storage, so the store can hold
// them in a flat array and hash them by value.
// What a name declared inside a type stands for.
enum class MemberKind : uint8_t {
  kField,
  kMethod,
  // A member of an enum: `Shape.Circle`.
  kVariant,
  // A state of a `machine`.
  kState,
  kSignal,
  // `deinit { ... }`: what the type does when a value of it goes away.
  // Not a method: nothing can call it, override it or name it.
  kDeinit,
  // `type Item` in an ability, which an implementation answers with
  // `type Item = T`. Its type is the bound in the ability and the answer
  // in the implementation (D212).
  kAssociatedType,
  // `derived var x: T => expr`: a field whose value is computed from other
  // fields and kept until one of them changes.
  kDerived,
  // Which state a `machine` is in: one field, written by the transitions
  // and read by the predicates.
  kMachineState,
  // What a signal holds: how many handlers there are, how many it has
  // room for, and where they are. Written by the compiler, like the ready
  // bit below and for the same reason.
  kSignalList,
  // The bit that says whether a derived field has been computed. Written
  // by the compiler, not by the source: a member table is the one place
  // every tier agrees about what a type holds, so the bit lives here
  // rather than in a layout of its own.
  kReadyBit,
};

// Whether a member takes space in the type: a field, a derived field's
// cached value, or the bit that says whether that cache is current.
// Everything else -- a method, a variant, a signal -- is code or a tag.
bool HasStorage(MemberKind kind);

// Which operator an `@Operator` ability lends its one method to (D239). Kept
// on the method's member so a use of `+` finds the ability by what it says
// and not by a name two libraries might spell alike.
enum class OperatorKind : uint8_t {
  kNone,
  kPlus,
  kMinus,
  kTimes,
  kDivide,
  kNegate,
  kEquals,
  kCompare,
};

// One name declared inside a named type. Member tables are part of the file
// resolution that produced them rather than of the type store, so that
// changing a field's type invalidates everything that reads it.
struct Member {
  static constexpr uint32_t kNoSource = 0xFFFFFFFFu;

  Symbol name;
  MemberKind kind = MemberKind::kField;
  TypeId type;
  // Where the member is declared, as a file index and an item tree index.
  // A method is not a file-scope name, so this is the only way a call site
  // can reach the function: two types may each have a `Sum`, and looking
  // one up by name would find either.
  uint32_t file = kNoSource;
  uint32_t item = kNoSource;
  // Which of several members one declaration stands for. A machine
  // declares one item and has a method per event and per state, so the
  // item alone does not identify them -- and a call that cannot tell two
  // methods apart calls whichever came first.
  uint32_t slot = 0;
  // Fields: whether the member may be assigned through.
  bool is_mutable = false;
  // Whether code outside the declaring module may name it.
  bool is_public = false;
  // Methods: whether a derived type may replace it, and whether this one
  // already replaces a base method.
  bool is_virtual = false;
  bool is_override = false;
  // Fields: `weak var`, which does not keep the object alive.
  bool is_weak = false;
  // Methods: `unsafe fun`, which charges its caller the `unsafe` effect (D236).
  bool is_unsafe = false;
  // Methods of an `@Operator` ability: the operator this one answers.
  OperatorKind operator_kind = OperatorKind::kNone;
  // Derived fields: which fields the expression reads, as indices into the
  // same table. A write to any of them is what makes the value stale, and
  // the list is here because the writer may be in another file.
  std::vector<uint32_t> depends_on;

  friend bool operator==(const Member& lhs, const Member& rhs) {
    return lhs.name == rhs.name && lhs.kind == rhs.kind &&
           lhs.type == rhs.type && lhs.file == rhs.file &&
           lhs.item == rhs.item && lhs.slot == rhs.slot &&
           lhs.is_mutable == rhs.is_mutable &&
           lhs.is_public == rhs.is_public &&
           lhs.is_virtual == rhs.is_virtual &&
           lhs.is_override == rhs.is_override &&
           lhs.is_weak == rhs.is_weak &&
           lhs.is_unsafe == rhs.is_unsafe &&
           lhs.operator_kind == rhs.operator_kind &&
           lhs.depends_on == rhs.depends_on;
  }
};

// One error a stated set names (D235). The name is the qualified
// `module.Name`; the tag is its hash; the detail is the class that boxes its
// payload, or invalid when it carries none.
struct ErrorMember {
  Symbol name;
  uint32_t tag = 0;
  TypeId detail;
};

enum class ErrorFit : uint8_t {
  kFits,
  kMissing,
  kPayloadIntoOpen,
};

// FNV-1a of the qualified error name, never zero: zero is the tag of success.
inline uint32_t ErrorTag(std::string_view qualified) {
  uint32_t hash = 0x811c9dc5u;
  for (char c : qualified) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x01000193u;
  }
  return hash == 0 ? 1u : hash;
}

struct Type {
  TypeKind kind = TypeKind::kError;

  // Integers: width and signedness.
  IntWidth int_width = IntWidth::k32;
  bool is_signed = true;
  // Floats: width.
  FloatWidth float_width = FloatWidth::k32;
  // Pointers and references: whether the target may be written.
  bool is_mutable = false;
  // Slices and arrays: whether the storage keeps one array per field.
  bool is_soa = false;
  // Named types: whether a value of it is a handle to a heap object, which
  // is what a `class` is and what a `struct` is not.
  //
  // Part of the type rather than of a file's member table, because the
  // verifier, the interpreter, the layout and both emitters all have to
  // agree about it and only one of them has a member table. It is also the
  // honest place: being a handle is what the type *is*.
  bool is_handle = false;
  // Named types: `extern class`, an object the host owns. A handle like any
  // class, counted by the host rather than by the runtime (D213).
  bool is_foreign = false;

  // Named types and parameters: the declared name.
  Symbol name;
  // Named types: which declaration introduced it, as a file plus an index
  // into that file's item tree. Storing the item tree index rather than a
  // syntax handle is what lets a type survive edits elsewhere in the file.
  uint32_t declaring_file = 0xFFFFFFFFu;
  uint32_t declaring_item = 0;

  // Optional, fallible, slice, array, pointer, reference: the inner type.
  // Function: the return type. Quantity: the underlying numeric type.
  //
  // Only meaningful for those kinds. Every Type is zeroed before use so that
  // interning can compare bytes, which leaves this reading as TypeId{0} -- a
  // real type -- rather than as an invalid id. Check the kind before
  // following it, or a walk over types will recurse forever.
  TypeId element;
  // Array: its length, or zero when the length was not a constant.
  uint64_t length = 0;

  // Instance arguments, tuple elements, function parameter types, error set
  // members. A range into the store's shared argument pool.
  uint32_t args_begin = 0;
  uint32_t args_count = 0;

  // Instance: the generic type being applied.
  TypeId base;

  // Quantity: the dimension product. A range into the store's factor pool.
  uint32_t factors_begin = 0;
  uint32_t factors_count = 0;

  // Function: whether the last parameter is variadic. Reserved for the FFI.
  bool is_variadic = false;
};

// Interns types and answers the structural questions about them.
//
// The store is per-compilation, like the string interner: a TypeId means
// nothing outside the store that produced it.
class TypeStore {
 public:
  TypeStore();

  TypeStore(const TypeStore&) = delete;
  TypeStore& operator=(const TypeStore&) = delete;

  // ------------------------------------------------------------ builtins --

  TypeId error() const { return error_; }
  TypeId void_type() const { return void_; }
  TypeId never() const { return never_; }
  TypeId bool_type() const { return bool_; }
  TypeId char_type() const { return char_; }
  TypeId string_type() const { return string_; }
  TypeId str() const { return str_; }
  // What a system asks the world for: `Query<mut Velocity, with Falling>`
  // is an instance of this, and its arguments are the access it declares.
  TypeId query() const { return query_; }
  // Interned up front so that something holding a const store can still
  // name them: a layout spelling out a class descriptor, an interpreter
  // reading a slice's two words.
  TypeId u32() const { return u32_; }
  TypeId u64() const { return u64_; }
  TypeId int_literal() const { return int_literal_; }
  TypeId float_literal() const { return float_literal_; }

  TypeId Int(IntWidth width, bool is_signed);
  TypeId Float(FloatWidth width);

  // `int` is `s64` and `float` is `f32` (D238); the aliases exist so gameplay code
  // does not have to spell a width it does not care about.
  TypeId default_int() { return Int(IntWidth::k64, true); }
  TypeId default_float() { return Float(FloatWidth::k32); }

  // ------------------------------------------------------- constructors --

  // Interns a base dimension by its declaration, so that `Length` declared
  // in one file is the same dimension everywhere it is imported. Dimension
  // ids are global for the same reason type ids are: a file-local index
  // would make two unrelated dimensions compare equal.
  DimensionId Dimension(Symbol name, uint32_t file, uint32_t item);

  // The name a dimension was declared under, or an invalid symbol.
  Symbol DimensionName(DimensionId dimension) const;

  // Interns an effect by its declaration. The built-in effects are interned
  // first, in BuiltinEffect order, so their ids are those constants.
  EffectId Effect(Symbol name, uint32_t file, uint32_t item);
  EffectId BuiltinEffectId(BuiltinEffect effect) const {
    return EffectId{static_cast<uint32_t>(effect)};
  }

  // The name an effect was declared under, or an invalid symbol for a
  // built-in -- those are spelled by BuiltinEffectName instead.
  Symbol EffectName(EffectId effect) const;

  // How an effect is written, built-in or declared.
  std::string EffectText(const StringInterner& interner,
                         EffectId effect) const;

  // Renders a row the way it would be written after `uses`.
  std::string Describe(const StringInterner& interner,
                       const EffectRow& row) const;

  bool IsBuiltinEffect(EffectId effect) const {
    return effect.is_valid() &&
           effect.index < static_cast<uint32_t>(BuiltinEffect::kCount);
  }

  bool IsForeignClass(TypeId type) const;
  TypeId Named(Symbol name, uint32_t file, uint32_t item,
               bool is_handle = false, bool is_foreign = false);
  // True when a value of |type| is a handle to a heap object: a `class`,
  // through any number of aliases.
  bool IsHandle(TypeId type) const;
  // |bounds| are the abilities and base types the argument has to satisfy.
  // Carried by the parameter itself, so a call site can check them without
  // reaching back into the declaring file.
  TypeId Parameter(Symbol name,
                   uint32_t owner,
                   uint32_t index,
                   const std::vector<TypeId>& bounds = {});
  TypeId Instance(TypeId base, const std::vector<TypeId>& args);
  TypeId Projection(TypeId owner, Symbol name);
  // |id| with every projection in it replaced by what |answer| says it is,
  // innermost first: `answer(owner, name)` sees an owner that has already
  // been answered. A type with no projection in it comes back unchanged.
  TypeId AnswerProjections(TypeId id, const std::function<TypeId(TypeId, Symbol)>& answer);
  bool MentionsProjection(TypeId id) const;
  // `Query<mut Velocity, with Falling>`: the components it names, and
  // whether the first of them is written. The flag is what `foreach` over
  // the query needs -- the rest of the access a system declares is read
  // off the syntax by the graph that schedules systems.
  TypeId Query(const std::vector<TypeId>& args, bool writes);
  TypeId Optional(TypeId element);
  // The optional of |element| if it was ever made, else the error type. Lets
  // a read-only user name the slot of a payload error, which a stated set made
  // when it was built (D235).
  TypeId FindOptional(TypeId element) const;
  TypeId Fallible(TypeId element, TypeId error_set);
  TypeId Slice(TypeId element, bool is_soa);
  TypeId Array(TypeId element, uint64_t length, bool is_soa);
  TypeId Tuple(const std::vector<TypeId>& elements);
  TypeId Function(const std::vector<TypeId>& params, TypeId result);
  TypeId Pointer(TypeId element, bool is_mutable);
  TypeId Reference(TypeId element, bool is_mutable);
  TypeId Quantity(TypeId numeric, const Dimensions& dimensions);
  TypeId ErrorSet(const std::vector<Symbol>& members);
  // A stated set (D235): sorted by tag, one entry per name. An empty list is
  // the open set, which is what `ErrorSet({})` also gives.
  TypeId StatedErrorSet(std::vector<ErrorMember> members);
  TypeId OpenErrorSet() { return ErrorSet({}); }

  // ------------------------------------------------------------ accessors --

  const Type& Get(TypeId id) const;
  TypeKind KindOf(TypeId id) const;
  size_t size() const { return types_.size(); }

  // Arguments of an instance, a tuple, a function or an error set.
  std::vector<TypeId> ArgumentsOf(TypeId id) const;
  // The dimension product of a quantity, or the empty product.
  Dimensions DimensionsOf(TypeId id) const;
  // Error set members, as interned names.
  std::vector<Symbol> ErrorMembersOf(TypeId id) const;
  std::vector<ErrorMember> StatedErrorsOf(TypeId id) const;
  bool IsOpenErrorSet(TypeId id) const;
  bool ErrorSetHasPayload(TypeId id) const;
  // Whether a value of |found| may stand where |expected| is wanted. Names the
  // members of |found| that |expected| lacks in |missing|.
  ErrorFit ErrorSetFits(TypeId expected, TypeId found, std::vector<Symbol>* missing) const;

  // ------------------------------------------------------------ questions --

  bool IsError(TypeId id) const { return KindOf(id) == TypeKind::kError; }
  bool IsNumeric(TypeId id) const;
  bool IsInteger(TypeId id) const;
  bool IsFloatingPoint(TypeId id) const;
  // True for a literal whose type is still open to its context.
  bool IsLiteral(TypeId id) const;
  bool IsOptional(TypeId id) const { return KindOf(id) == TypeKind::kOptional; }
  bool IsFallible(TypeId id) const { return KindOf(id) == TypeKind::kFallible; }

  // The type inside an optional, fallible, slice, array, pointer, reference
  // or quantity, or an invalid id.
  TypeId ElementOf(TypeId id) const;
  // Strips every layer of `?` and `!`, which is what a diagnostic about the
  // underlying value wants.
  TypeId StripWrappers(TypeId id) const;

  // True when |id| is a type parameter, or contains one anywhere inside it.
  // A signature that does not is already concrete and needs no inference.
  bool MentionsParameter(TypeId id) const;
  // True when |id| nests more than |limit| levels of element, base or
  // argument. Stops descending at the limit, so it is safe on a type that
  // polymorphic recursion made arbitrarily deep (D225).
  bool NestsDeeperThan(TypeId id, uint32_t limit) const;
  // The type parameters |id| mentions, in the order they appear. What a
  // call with explicit type arguments matches them against: a signature
  // may mention a parameter only in its result, and then there is nothing
  // to infer from.
  void CollectParameters(TypeId id, std::vector<TypeId>* out) const;

  // One type parameter and what it was inferred to stand for.
  using Substitution = std::vector<std::pair<TypeId, TypeId>>;

  // Matches |pattern| against |actual|, recording what each type parameter
  // in |pattern| has to be for the two to agree. Returns false when they
  // cannot agree at all; a parameter already bound to something else is the
  // interesting case.
  bool Unify(TypeId pattern, TypeId actual, Substitution* out) const;

  // Rebuilds |id| with every type parameter in |out| replaced. A parameter
  // that was never inferred is left alone.
  TypeId Substitute(TypeId id, const Substitution& substitution);

  // Width in bits of an integer or float type, or zero.
  uint32_t BitWidth(TypeId id) const;

  // Renders a type the way the user wrote it: `Array<int>?`, `[float; 3]`,
  // `Length/Time`.
  std::string Describe(const StringInterner& interner, TypeId id) const;

 private:
  // Interning key: a type's bytes plus the contents of its ranges, since two
  // structurally equal types must land on the same id.
  struct Key {
    Type type;
    std::vector<TypeId> args;
    std::vector<Dimensions::Factor> factors;
    std::vector<Symbol> names;
    std::vector<uint32_t> tags;
    std::vector<TypeId> details;

    friend bool operator==(const Key& lhs, const Key& rhs) {
      // The ranges are compared through the key's own copies, so two
      // structurally equal types compare equal before either is stored.
      return lhs.SameType(rhs) && lhs.args == rhs.args &&
             lhs.factors == rhs.factors && lhs.names == rhs.names &&
             lhs.tags == rhs.tags && lhs.details == rhs.details;
    }

    bool SameType(const Key& other) const;
  };
  struct KeyHash {
    size_t operator()(const Key& key) const;
  };

  TypeId Intern(Key key);

  std::vector<Type> types_;
  std::vector<TypeId> args_;
  std::vector<Dimensions::Factor> factors_;
  std::vector<Symbol> names_;
  std::vector<uint32_t> tags_;
  std::vector<TypeId> details_;
  std::unordered_map<Key, uint32_t, KeyHash> interned_;

  TypeId error_;
  TypeId void_;
  TypeId never_;
  TypeId bool_;
  TypeId char_;
  TypeId string_;
  TypeId str_;
  TypeId query_;
  TypeId u32_;
  TypeId u64_;
  TypeId int_literal_;
  TypeId float_literal_;

  // Base dimensions, in declaration order. The index is the DimensionId.
  struct DimensionEntry {
    Symbol name;
    uint32_t file = 0;
    uint32_t item = 0;
  };
  std::vector<DimensionEntry> dimensions_;

  // Effects, in declaration order with the built-ins first. The index is the
  // EffectId.
  struct EffectEntry {
    Symbol name;
    uint32_t file = 0;
    uint32_t item = 0;
  };
  std::vector<EffectEntry> effects_;
};

}  // namespace wolang

#endif  // WOLANG_SEMA_TYPE_H_
