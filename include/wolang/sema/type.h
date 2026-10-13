// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SEMA_TYPE_H_
#define WOLANG_SEMA_TYPE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/sema/dimension.h"

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
  // A declared struct, class, enum, trait, component or tag.
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
};

// One name declared inside a named type. Member tables are part of the file
// resolution that produced them rather than of the type store, so that
// changing a field's type invalidates everything that reads it.
struct Member {
  Symbol name;
  MemberKind kind = MemberKind::kField;
  TypeId type;
  // Fields: whether the member may be assigned through.
  bool is_mutable = false;
  // Whether code outside the declaring module may name it.
  bool is_public = false;

  friend bool operator==(const Member& lhs, const Member& rhs) {
    return lhs.name == rhs.name && lhs.kind == rhs.kind &&
           lhs.type == rhs.type && lhs.is_mutable == rhs.is_mutable &&
           lhs.is_public == rhs.is_public;
  }
};

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

  // Named types and parameters: the declared name.
  Symbol name;
  // Named types: which declaration introduced it, as a file plus an index
  // into that file's item tree. Storing the item tree index rather than a
  // syntax handle is what lets a type survive edits elsewhere in the file.
  uint32_t declaring_file = 0xFFFFFFFFu;
  uint32_t declaring_item = 0;

  // Optional, fallible, slice, array, pointer, reference: the inner type.
  // Function: the return type. Quantity: the underlying numeric type.
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
  TypeId int_literal() const { return int_literal_; }
  TypeId float_literal() const { return float_literal_; }

  TypeId Int(IntWidth width, bool is_signed);
  TypeId Float(FloatWidth width);

  // `int` is `i32` and `float` is `f32`; the aliases exist so gameplay code
  // does not have to spell a width it does not care about.
  TypeId default_int() { return Int(IntWidth::k32, true); }
  TypeId default_float() { return Float(FloatWidth::k32); }

  // ------------------------------------------------------- constructors --

  // Interns a base dimension by its declaration, so that `Length` declared
  // in one file is the same dimension everywhere it is imported. Dimension
  // ids are global for the same reason type ids are: a file-local index
  // would make two unrelated dimensions compare equal.
  DimensionId Dimension(Symbol name, uint32_t file, uint32_t item);

  // The name a dimension was declared under, or an invalid symbol.
  Symbol DimensionName(DimensionId dimension) const;

  TypeId Named(Symbol name, uint32_t file, uint32_t item);
  TypeId Parameter(Symbol name, uint32_t owner, uint32_t index);
  TypeId Instance(TypeId base, const std::vector<TypeId>& args);
  TypeId Optional(TypeId element);
  TypeId Fallible(TypeId element, TypeId error_set);
  TypeId Slice(TypeId element, bool is_soa);
  TypeId Array(TypeId element, uint64_t length, bool is_soa);
  TypeId Tuple(const std::vector<TypeId>& elements);
  TypeId Function(const std::vector<TypeId>& params, TypeId result);
  TypeId Pointer(TypeId element, bool is_mutable);
  TypeId Reference(TypeId element, bool is_mutable);
  TypeId Quantity(TypeId numeric, const Dimensions& dimensions);
  TypeId ErrorSet(const std::vector<Symbol>& members);

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

    friend bool operator==(const Key& lhs, const Key& rhs) {
      // The ranges are compared through the key's own copies, so two
      // structurally equal types compare equal before either is stored.
      return lhs.SameType(rhs) && lhs.args == rhs.args &&
             lhs.factors == rhs.factors && lhs.names == rhs.names;
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
  std::unordered_map<Key, uint32_t, KeyHash> interned_;

  TypeId error_;
  TypeId void_;
  TypeId never_;
  TypeId bool_;
  TypeId char_;
  TypeId string_;
  TypeId str_;
  TypeId int_literal_;
  TypeId float_literal_;

  // Base dimensions, in declaration order. The index is the DimensionId.
  struct DimensionEntry {
    Symbol name;
    uint32_t file = 0;
    uint32_t item = 0;
  };
  std::vector<DimensionEntry> dimensions_;
};

}  // namespace wolang

#endif  // WOLANG_SEMA_TYPE_H_
