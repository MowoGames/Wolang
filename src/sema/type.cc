// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/type.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "wolang/base/check.h"

namespace wolang {
namespace {

uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  uint64_t hash = seed;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3;
  }
  return hash;
}

}  // namespace

std::string_view TypeKindName(TypeKind kind) {
  switch (kind) {
    case TypeKind::kError:
      return "error";
    case TypeKind::kVoid:
      return "void";
    case TypeKind::kNever:
      return "never";
    case TypeKind::kBool:
      return "bool";
    case TypeKind::kChar:
      return "char";
    case TypeKind::kInt:
      return "int";
    case TypeKind::kFloat:
      return "float";
    case TypeKind::kIntLiteral:
      return "integer literal";
    case TypeKind::kFloatLiteral:
      return "float literal";
    case TypeKind::kString:
      return "String";
    case TypeKind::kStr:
      return "str";
    case TypeKind::kNamed:
      return "named";
    case TypeKind::kParameter:
      return "type parameter";
    case TypeKind::kInstance:
      return "instance";
    case TypeKind::kOptional:
      return "optional";
    case TypeKind::kFallible:
      return "fallible";
    case TypeKind::kSlice:
      return "slice";
    case TypeKind::kArray:
      return "array";
    case TypeKind::kTuple:
      return "tuple";
    case TypeKind::kFunction:
      return "function";
    case TypeKind::kPointer:
      return "pointer";
    case TypeKind::kReference:
      return "reference";
    case TypeKind::kQuantity:
      return "quantity";
    case TypeKind::kErrorSet:
      return "error set";
  }
  return "error";
}

bool TypeStore::Key::SameType(const Key& other) const {
  // Every byte of Type, padding included, is zeroed before use, so a plain
  // comparison is a complete structural one.
  return std::memcmp(&type, &other.type, sizeof(Type)) == 0;
}

size_t TypeStore::KeyHash::operator()(const Key& key) const {
  uint64_t hash = HashBytes(&key.type, sizeof(Type), 0xcbf29ce484222325);
  for (TypeId id : key.args) {
    hash = HashBytes(&id.index, sizeof(id.index), hash);
  }
  for (const Dimensions::Factor& factor : key.factors) {
    hash = HashBytes(&factor, sizeof(factor), hash);
  }
  for (Symbol name : key.names) {
    hash = HashBytes(&name.index, sizeof(name.index), hash);
  }
  return static_cast<size_t>(hash);
}

TypeStore::TypeStore() {
  types_.reserve(64);

  const auto simple = [this](TypeKind kind) {
    Key key;
    // The struct is memcmp'd, so every byte including padding must be set.
    std::memset(&key.type, 0, sizeof(key.type));
    key.type.kind = kind;
    return Intern(std::move(key));
  };

  error_ = simple(TypeKind::kError);
  void_ = simple(TypeKind::kVoid);
  never_ = simple(TypeKind::kNever);
  bool_ = simple(TypeKind::kBool);
  char_ = simple(TypeKind::kChar);
  string_ = simple(TypeKind::kString);
  str_ = simple(TypeKind::kStr);
  int_literal_ = simple(TypeKind::kIntLiteral);
  float_literal_ = simple(TypeKind::kFloatLiteral);
}

TypeId TypeStore::Intern(Key key) {
  const auto it = interned_.find(key);
  if (it != interned_.end()) {
    return TypeId{it->second};
  }

  Type type = key.type;
  if (!key.args.empty()) {
    type.args_begin = static_cast<uint32_t>(args_.size());
    type.args_count = static_cast<uint32_t>(key.args.size());
    args_.insert(args_.end(), key.args.begin(), key.args.end());
  }
  if (!key.factors.empty()) {
    type.factors_begin = static_cast<uint32_t>(factors_.size());
    type.factors_count = static_cast<uint32_t>(key.factors.size());
    factors_.insert(factors_.end(), key.factors.begin(), key.factors.end());
  }
  if (!key.names.empty()) {
    type.args_begin = static_cast<uint32_t>(names_.size());
    type.args_count = static_cast<uint32_t>(key.names.size());
    names_.insert(names_.end(), key.names.begin(), key.names.end());
  }

  const auto index = static_cast<uint32_t>(types_.size());
  types_.push_back(type);
  interned_.emplace(std::move(key), index);
  return TypeId{index};
}

TypeId TypeStore::Int(IntWidth width, bool is_signed) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kInt;
  key.type.int_width = width;
  key.type.is_signed = is_signed;
  return Intern(std::move(key));
}

TypeId TypeStore::Float(FloatWidth width) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kFloat;
  key.type.float_width = width;
  return Intern(std::move(key));
}

TypeId TypeStore::Named(Symbol name, uint32_t file, uint32_t item) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kNamed;
  key.type.name = name;
  key.type.declaring_file = file;
  key.type.declaring_item = item;
  return Intern(std::move(key));
}

TypeId TypeStore::Parameter(Symbol name, uint32_t owner, uint32_t index) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kParameter;
  key.type.name = name;
  key.type.declaring_file = owner;
  key.type.declaring_item = index;
  return Intern(std::move(key));
}

TypeId TypeStore::Instance(TypeId base, const std::vector<TypeId>& args) {
  if (args.empty()) {
    return base;
  }
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kInstance;
  key.type.base = base;
  key.args = args;
  return Intern(std::move(key));
}

TypeId TypeStore::Optional(TypeId element) {
  // `T??` is the same type as `T?`: there is one kind of absence, so wrapping
  // twice says nothing new.
  if (KindOf(element) == TypeKind::kOptional) {
    return element;
  }
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kOptional;
  key.type.element = element;
  return Intern(std::move(key));
}

TypeId TypeStore::Fallible(TypeId element, TypeId error_set) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kFallible;
  key.type.element = element;
  key.type.base = error_set;
  return Intern(std::move(key));
}

TypeId TypeStore::Slice(TypeId element, bool is_soa) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kSlice;
  key.type.element = element;
  key.type.is_soa = is_soa;
  return Intern(std::move(key));
}

TypeId TypeStore::Array(TypeId element, uint64_t length, bool is_soa) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kArray;
  key.type.element = element;
  key.type.length = length;
  key.type.is_soa = is_soa;
  return Intern(std::move(key));
}

TypeId TypeStore::Tuple(const std::vector<TypeId>& elements) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kTuple;
  key.args = elements;
  return Intern(std::move(key));
}

TypeId TypeStore::Function(const std::vector<TypeId>& params, TypeId result) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kFunction;
  key.type.element = result;
  key.args = params;
  return Intern(std::move(key));
}

TypeId TypeStore::Pointer(TypeId element, bool is_mutable) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kPointer;
  key.type.element = element;
  key.type.is_mutable = is_mutable;
  return Intern(std::move(key));
}

TypeId TypeStore::Reference(TypeId element, bool is_mutable) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kReference;
  key.type.element = element;
  key.type.is_mutable = is_mutable;
  return Intern(std::move(key));
}

TypeId TypeStore::Quantity(TypeId numeric, const Dimensions& dimensions) {
  // A dimensionless quantity is just its numeric type, which is what makes
  // `10m / 2m` usable anywhere a plain number is.
  if (dimensions.is_dimensionless()) {
    return numeric;
  }
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kQuantity;
  key.type.element = numeric;
  key.factors = dimensions.factors();
  return Intern(std::move(key));
}

TypeId TypeStore::ErrorSet(const std::vector<Symbol>& members) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kErrorSet;
  key.names = members;
  return Intern(std::move(key));
}

const Type& TypeStore::Get(TypeId id) const {
  WOLANG_DCHECK(id.is_valid() && id.index < types_.size());
  if (!id.is_valid() || id.index >= types_.size()) {
    return types_[error_.index];
  }
  return types_[id.index];
}

TypeKind TypeStore::KindOf(TypeId id) const {
  if (!id.is_valid() || id.index >= types_.size()) {
    return TypeKind::kError;
  }
  return types_[id.index].kind;
}

std::vector<TypeId> TypeStore::ArgumentsOf(TypeId id) const {
  const Type& type = Get(id);
  if (type.kind == TypeKind::kErrorSet) {
    return {};
  }
  std::vector<TypeId> result;
  result.reserve(type.args_count);
  for (uint32_t i = 0; i < type.args_count; ++i) {
    result.push_back(args_[type.args_begin + i]);
  }
  return result;
}

Dimensions TypeStore::DimensionsOf(TypeId id) const {
  const Type& type = Get(id);
  Dimensions result;
  for (uint32_t i = 0; i < type.factors_count; ++i) {
    const Dimensions::Factor& factor = factors_[type.factors_begin + i];
    result = result.Multiply(
        Dimensions::Base(factor.dimension).Power(factor.exponent));
  }
  return result;
}

std::vector<Symbol> TypeStore::ErrorMembersOf(TypeId id) const {
  const Type& type = Get(id);
  if (type.kind != TypeKind::kErrorSet) {
    return {};
  }
  std::vector<Symbol> result;
  result.reserve(type.args_count);
  for (uint32_t i = 0; i < type.args_count; ++i) {
    result.push_back(names_[type.args_begin + i]);
  }
  return result;
}

bool TypeStore::IsInteger(TypeId id) const {
  const TypeKind kind = KindOf(id);
  return kind == TypeKind::kInt || kind == TypeKind::kIntLiteral;
}

bool TypeStore::IsFloatingPoint(TypeId id) const {
  const TypeKind kind = KindOf(id);
  return kind == TypeKind::kFloat || kind == TypeKind::kFloatLiteral;
}

bool TypeStore::IsNumeric(TypeId id) const {
  if (KindOf(id) == TypeKind::kQuantity) {
    return IsNumeric(ElementOf(id));
  }
  return IsInteger(id) || IsFloatingPoint(id);
}

bool TypeStore::IsLiteral(TypeId id) const {
  const TypeKind kind = KindOf(id);
  return kind == TypeKind::kIntLiteral || kind == TypeKind::kFloatLiteral;
}

TypeId TypeStore::ElementOf(TypeId id) const {
  switch (KindOf(id)) {
    case TypeKind::kOptional:
    case TypeKind::kFallible:
    case TypeKind::kSlice:
    case TypeKind::kArray:
    case TypeKind::kPointer:
    case TypeKind::kReference:
    case TypeKind::kQuantity:
    case TypeKind::kFunction:
      return Get(id).element;
    default:
      return TypeId{};
  }
}

TypeId TypeStore::StripWrappers(TypeId id) const {
  TypeId current = id;
  for (;;) {
    const TypeKind kind = KindOf(current);
    if (kind != TypeKind::kOptional && kind != TypeKind::kFallible) {
      return current;
    }
    current = Get(current).element;
  }
}

uint32_t TypeStore::BitWidth(TypeId id) const {
  const Type& type = Get(id);
  if (type.kind == TypeKind::kInt) {
    switch (type.int_width) {
      case IntWidth::k8:
        return 8;
      case IntWidth::k16:
        return 16;
      case IntWidth::k32:
        return 32;
      case IntWidth::k64:
        return 64;
    }
  }
  if (type.kind == TypeKind::kFloat) {
    return type.float_width == FloatWidth::k32 ? 32 : 64;
  }
  return 0;
}

std::string TypeStore::Describe(
    const StringInterner& interner,
    TypeId id,
    const std::vector<Symbol>& dimension_names) const {
  if (!id.is_valid()) {
    return "<unknown>";
  }
  const Type& type = Get(id);

  switch (type.kind) {
    case TypeKind::kError:
      return "<error>";
    case TypeKind::kVoid:
      return "()";
    case TypeKind::kNever:
      return "never";
    case TypeKind::kBool:
      return "bool";
    case TypeKind::kChar:
      return "char";
    case TypeKind::kString:
      return "String";
    case TypeKind::kStr:
      return "str";
    case TypeKind::kIntLiteral:
      return "integer literal";
    case TypeKind::kFloatLiteral:
      return "float literal";

    case TypeKind::kInt: {
      std::string text = type.is_signed ? "i" : "u";
      text.append(std::to_string(BitWidth(id)));
      return text;
    }
    case TypeKind::kFloat:
      return type.float_width == FloatWidth::k32 ? "f32" : "f64";

    case TypeKind::kNamed:
    case TypeKind::kParameter:
      return std::string(interner.Text(type.name));

    case TypeKind::kInstance: {
      std::string text = Describe(interner, type.base, dimension_names);
      text.push_back('<');
      bool first = true;
      for (TypeId arg : ArgumentsOf(id)) {
        if (!first) {
          text.append(", ");
        }
        first = false;
        text.append(Describe(interner, arg, dimension_names));
      }
      text.push_back('>');
      return text;
    }

    case TypeKind::kOptional:
      return Describe(interner, type.element, dimension_names) + "?";
    case TypeKind::kFallible:
      return Describe(interner, type.element, dimension_names) + "!";

    case TypeKind::kSlice: {
      std::string text = type.is_soa ? "soa [" : "[";
      text.append(Describe(interner, type.element, dimension_names));
      text.push_back(']');
      return text;
    }
    case TypeKind::kArray: {
      std::string text = type.is_soa ? "soa [" : "[";
      text.append(Describe(interner, type.element, dimension_names));
      text.append("; ");
      text.append(std::to_string(type.length));
      text.push_back(']');
      return text;
    }

    case TypeKind::kTuple: {
      std::string text = "(";
      bool first = true;
      for (TypeId element : ArgumentsOf(id)) {
        if (!first) {
          text.append(", ");
        }
        first = false;
        text.append(Describe(interner, element, dimension_names));
      }
      text.push_back(')');
      return text;
    }

    case TypeKind::kFunction: {
      std::string text = "fun(";
      bool first = true;
      for (TypeId param : ArgumentsOf(id)) {
        if (!first) {
          text.append(", ");
        }
        first = false;
        text.append(Describe(interner, param, dimension_names));
      }
      text.push_back(')');
      if (type.element.is_valid() &&
          KindOf(type.element) != TypeKind::kVoid) {
        text.append(" -> ");
        text.append(Describe(interner, type.element, dimension_names));
      }
      return text;
    }

    case TypeKind::kPointer:
      return (type.is_mutable ? "*mut " : "*") +
             Describe(interner, type.element, dimension_names);
    case TypeKind::kReference:
      return (type.is_mutable ? "&mut " : "&") +
             Describe(interner, type.element, dimension_names);

    case TypeKind::kQuantity: {
      // The dimension is what the reader cares about; the carrier is only
      // mentioned when it is not the default.
      std::string text =
          DimensionsOf(id).Describe(interner, dimension_names);
      if (type.element.is_valid() &&
          !(KindOf(type.element) == TypeKind::kFloat &&
            Get(type.element).float_width == FloatWidth::k32)) {
        text.push_back(' ');
        text.push_back('(');
        text.append(Describe(interner, type.element, dimension_names));
        text.push_back(')');
      }
      return text;
    }

    case TypeKind::kErrorSet: {
      std::string text;
      bool first = true;
      for (Symbol member : ErrorMembersOf(id)) {
        if (!first) {
          text.append(" | ");
        }
        first = false;
        text.append(interner.Text(member));
      }
      return text.empty() ? "error{}" : text;
    }
  }
  return "<error>";
}

}  // namespace wolang
