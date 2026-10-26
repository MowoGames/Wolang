// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/sema/type.h"

#include <algorithm>

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
      return "Text";
    case TypeKind::kStr:
      return "text";
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
    case TypeKind::kProjection:
      return "associated type";
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

  u32_ = Int(IntWidth::k32, /*is_signed=*/false);
  u64_ = Int(IntWidth::k64, /*is_signed=*/false);

  // A named type with no declaration anywhere: what `Query<...>` is an
  // instance of. Interned like everything else, so two queries over the
  // same components are the same type.
  {
    Key key;
    std::memset(&key.type, 0, sizeof(key.type));
    key.type.kind = TypeKind::kNamed;
    key.type.declaring_file = 0xFFFFFFFFu;
    key.type.declaring_item = 0xFFFFFFFEu;
    query_ = Intern(std::move(key));
  }

  // The built-in effects occupy the first ids, in BuiltinEffect order, so
  // that testing for one is an index comparison. They are spelled by
  // BuiltinEffectName rather than by a symbol, which is what keeps the type
  // store independent of any interner.
  effects_.resize(static_cast<size_t>(BuiltinEffect::kCount));
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

bool HasStorage(MemberKind kind) {
  return kind == MemberKind::kField || kind == MemberKind::kDerived ||
         kind == MemberKind::kReadyBit ||
         kind == MemberKind::kSignalList ||
         kind == MemberKind::kMachineState;
}

TypeId TypeStore::Named(Symbol name,
                        uint32_t file,
                        uint32_t item,
                        bool is_handle) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kNamed;
  key.type.name = name;
  key.type.declaring_file = file;
  key.type.declaring_item = item;
  key.type.is_handle = is_handle;
  return Intern(std::move(key));
}

bool TypeStore::IsHandle(TypeId type) const {
  TypeId settled = StripWrappers(type);
  // `List<int>` is a handle exactly when `List` is: an instance is the
  // declaration with its parameters bound, and whether a value of it is an
  // object somewhere or the object itself is not something binding a
  // parameter changes.
  if (KindOf(settled) == TypeKind::kInstance) {
    settled = Get(settled).base;
  }
  return KindOf(settled) == TypeKind::kNamed && Get(settled).is_handle;
}

TypeId TypeStore::Parameter(Symbol name,
                            uint32_t owner,
                            uint32_t index,
                            const std::vector<TypeId>& bounds) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kParameter;
  key.type.name = name;
  key.type.declaring_file = owner;
  key.type.declaring_item = index;
  key.args = bounds;
  return Intern(std::move(key));
}

TypeId TypeStore::Projection(TypeId owner, Symbol name) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kProjection;
  key.type.base = owner;
  key.type.name = name;
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

TypeId TypeStore::Query(const std::vector<TypeId>& args, bool writes) {
  Key key;
  std::memset(&key.type, 0, sizeof(key.type));
  key.type.kind = TypeKind::kInstance;
  key.type.base = query_;
  key.type.element = args.empty() ? TypeId{} : args.front();
  key.type.is_mutable = writes;
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
  // Asking for the arguments of a type that is not there is a legitimate
  // "none": a declaration whose signature never resolved still gets walked.
  if (!id.is_valid()) {
    return {};
  }
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

EffectId TypeStore::Effect(Symbol name, uint32_t file, uint32_t item) {
  for (uint32_t i = static_cast<uint32_t>(BuiltinEffect::kCount);
       i < effects_.size(); ++i) {
    const EffectEntry& entry = effects_[i];
    if (entry.name == name && entry.file == file && entry.item == item) {
      return EffectId{i};
    }
  }
  effects_.push_back(EffectEntry{name, file, item});
  return EffectId{static_cast<uint32_t>(effects_.size() - 1)};
}

Symbol TypeStore::EffectName(EffectId effect) const {
  if (!effect.is_valid() || effect.index >= effects_.size()) {
    return Symbol{};
  }
  return effects_[effect.index].name;
}

std::string TypeStore::EffectText(const StringInterner& interner,
                                  EffectId effect) const {
  if (IsBuiltinEffect(effect)) {
    return std::string(
        BuiltinEffectName(static_cast<BuiltinEffect>(effect.index)));
  }
  const Symbol name = EffectName(effect);
  return name.is_valid() ? std::string(interner.Text(name)) : "?";
}

std::string TypeStore::Describe(const StringInterner& interner,
                                const EffectRow& row) const {
  if (row.empty()) {
    return "no effects";
  }
  std::string result;
  for (EffectId effect : row.effects()) {
    if (!result.empty()) {
      result.append(", ");
    }
    result.append(EffectText(interner, effect));
  }
  return result;
}

Symbol TypeStore::DimensionName(DimensionId dimension) const {
  if (!dimension.is_valid() || dimension.index >= dimensions_.size()) {
    return Symbol{};
  }
  return dimensions_[dimension.index].name;
}

DimensionId TypeStore::Dimension(Symbol name, uint32_t file, uint32_t item) {
  for (uint32_t i = 0; i < dimensions_.size(); ++i) {
    const DimensionEntry& entry = dimensions_[i];
    if (entry.name == name && entry.file == file && entry.item == item) {
      return DimensionId{i};
    }
  }
  dimensions_.push_back(DimensionEntry{name, file, item});
  return DimensionId{static_cast<uint32_t>(dimensions_.size() - 1)};
}

namespace {

// A zeroed Type has `element` and `base` reading as TypeId{0}, which is a
// real type rather than an invalid id, so neither field may be followed
// without first checking that this kind actually uses it.
bool CarriesElement(TypeKind kind) {
  switch (kind) {
    case TypeKind::kOptional:
    case TypeKind::kFallible:
    case TypeKind::kSlice:
    case TypeKind::kArray:
    case TypeKind::kPointer:
    case TypeKind::kReference:
    case TypeKind::kFunction:
    case TypeKind::kQuantity:
      return true;
    default:
      return false;
  }
}

bool CarriesBase(TypeKind kind) {
  return kind == TypeKind::kInstance || kind == TypeKind::kProjection;
}

}  // namespace

bool TypeStore::MentionsProjection(TypeId id) const {
  if (!id.is_valid()) {
    return false;
  }
  const Type& type = Get(id);
  if (type.kind == TypeKind::kProjection) {
    return true;
  }
  if (CarriesElement(type.kind) && MentionsProjection(type.element)) {
    return true;
  }
  if (CarriesBase(type.kind) && MentionsProjection(type.base)) {
    return true;
  }
  for (TypeId arg : ArgumentsOf(id)) {
    if (MentionsProjection(arg)) {
      return true;
    }
  }
  return false;
}

TypeId TypeStore::AnswerProjections(TypeId id, const std::function<TypeId(TypeId, Symbol)>& answer) {
  if (!MentionsProjection(id)) {
    return id;
  }

  const Type type = Get(id);
  if (type.kind == TypeKind::kProjection) {
    return answer(AnswerProjections(type.base, answer), type.name);
  }

  std::vector<TypeId> args;
  for (TypeId arg : ArgumentsOf(id)) {
    args.push_back(AnswerProjections(arg, answer));
  }
  const TypeId element = CarriesElement(type.kind) ? AnswerProjections(type.element, answer) : type.element;
  switch (type.kind) {
    case TypeKind::kOptional:
      return Optional(element);
    case TypeKind::kFallible:
      return Fallible(element, type.base);
    case TypeKind::kSlice:
      return Slice(element, type.is_soa);
    case TypeKind::kArray:
      return Array(element, type.length, type.is_soa);
    case TypeKind::kPointer:
      return Pointer(element, type.is_mutable);
    case TypeKind::kReference:
      return Reference(element, type.is_mutable);
    case TypeKind::kTuple:
      return Tuple(args);
    case TypeKind::kFunction:
      return Function(args, element);
    case TypeKind::kInstance:
      return Instance(AnswerProjections(type.base, answer), args);
    default:
      return id;
  }
}

bool TypeStore::MentionsParameter(TypeId id) const {
  if (!id.is_valid()) {
    return false;
  }
  const Type& type = Get(id);
  if (type.kind == TypeKind::kParameter) {
    return true;
  }
  if (CarriesElement(type.kind) && MentionsParameter(type.element)) {
    return true;
  }
  if (CarriesBase(type.kind) && MentionsParameter(type.base)) {
    return true;
  }
  for (TypeId arg : ArgumentsOf(id)) {
    if (MentionsParameter(arg)) {
      return true;
    }
  }
  return false;
}

void TypeStore::CollectParameters(TypeId id,
                                  std::vector<TypeId>* out) const {
  if (!id.is_valid()) {
    return;
  }
  const Type& type = Get(id);
  if (type.kind == TypeKind::kParameter) {
    if (std::find(out->begin(), out->end(), id) == out->end()) {
      out->push_back(id);
    }
    return;
  }
  // Arguments before the element, so that a function's parameters come in
  // the order they were written and its result last.
  for (TypeId argument : ArgumentsOf(id)) {
    CollectParameters(argument, out);
  }
  if (CarriesBase(type.kind)) {
    CollectParameters(type.base, out);
  }
  if (CarriesElement(type.kind)) {
    CollectParameters(type.element, out);
  }
}

bool TypeStore::Unify(TypeId pattern,
                      TypeId actual,
                      Substitution* out) const {
  if (!pattern.is_valid() || !actual.is_valid()) {
    return true;
  }
  if (pattern == actual) {
    return true;
  }

  const Type& left = Get(pattern);
  // Nothing can be learned about `C` from what `C.Item` turned out to be.
  if (left.kind == TypeKind::kProjection) {
    return true;
  }
  if (left.kind == TypeKind::kParameter) {
    for (const auto& [parameter, bound] : *out) {
      if (parameter == pattern) {
        // Two arguments disagreeing about the same parameter is the error
        // worth reporting, and the caller reports it as a mismatch.
        return bound == actual;
      }
    }
    out->emplace_back(pattern, actual);
    return true;
  }

  const Type& right = Get(actual);
  if (left.kind != right.kind) {
    return false;
  }
  if (CarriesElement(left.kind) &&
      !Unify(left.element, right.element, out)) {
    return false;
  }
  if (CarriesBase(left.kind) && !Unify(left.base, right.base, out)) {
    return false;
  }

  const std::vector<TypeId> left_args = ArgumentsOf(pattern);
  const std::vector<TypeId> right_args = ArgumentsOf(actual);
  if (left_args.size() != right_args.size()) {
    return false;
  }
  for (size_t i = 0; i < left_args.size(); ++i) {
    if (!Unify(left_args[i], right_args[i], out)) {
      return false;
    }
  }
  return true;
}

TypeId TypeStore::Substitute(TypeId id, const Substitution& substitution) {
  if (!id.is_valid() || substitution.empty() || !MentionsParameter(id)) {
    return id;
  }

  const Type type = Get(id);
  if (type.kind == TypeKind::kParameter) {
    for (const auto& [parameter, bound] : substitution) {
      if (parameter == id) {
        return bound;
      }
    }
    return id;
  }

  std::vector<TypeId> args;
  for (TypeId arg : ArgumentsOf(id)) {
    args.push_back(Substitute(arg, substitution));
  }
  const TypeId element = CarriesElement(type.kind)
                             ? Substitute(type.element, substitution)
                             : type.element;

  switch (type.kind) {
    case TypeKind::kOptional:
      return Optional(element);
    case TypeKind::kFallible:
      return Fallible(element, type.base);
    case TypeKind::kSlice:
      return Slice(element, type.is_soa);
    case TypeKind::kArray:
      return Array(element, type.length, type.is_soa);
    case TypeKind::kPointer:
      return Pointer(element, type.is_mutable);
    case TypeKind::kReference:
      return Reference(element, type.is_mutable);
    case TypeKind::kTuple:
      return Tuple(args);
    case TypeKind::kFunction:
      return Function(args, element);
    case TypeKind::kInstance:
      return Instance(Substitute(type.base, substitution), args);
    case TypeKind::kProjection:
      return Projection(Substitute(type.base, substitution), type.name);
    default:
      return id;
  }
}

std::string TypeStore::Describe(const StringInterner& interner,
                                TypeId id) const {
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
      return "Text";
    case TypeKind::kStr:
      return "text";
    case TypeKind::kIntLiteral:
      return "integer literal";
    case TypeKind::kFloatLiteral:
      return "float literal";

    case TypeKind::kInt: {
      std::string text = type.is_signed ? "s" : "u";
      text.append(std::to_string(BitWidth(id)));
      return text;
    }
    case TypeKind::kFloat:
      return type.float_width == FloatWidth::k32 ? "f32" : "f64";

    case TypeKind::kNamed:
    case TypeKind::kParameter:
      return std::string(interner.Text(type.name));

    case TypeKind::kProjection:
      return Describe(interner, type.base) + "." + std::string(interner.Text(type.name));

    case TypeKind::kInstance: {
      std::string text = Describe(interner, type.base);
      text.push_back('<');
      bool first = true;
      for (TypeId arg : ArgumentsOf(id)) {
        if (!first) {
          text.append(", ");
        }
        first = false;
        text.append(Describe(interner, arg));
      }
      text.push_back('>');
      return text;
    }

    case TypeKind::kOptional:
      return Describe(interner, type.element) + "?";
    case TypeKind::kFallible:
      return Describe(interner, type.element) + "!";

    // `[]T` and `[N]T` read left to right: a slice of T, an array of N of
    // them. The brackets say what the shape is before the element does.
    case TypeKind::kSlice: {
      std::string text = type.is_soa ? "soa []" : "[]";
      text.append(Describe(interner, type.element));
      return text;
    }
    case TypeKind::kArray: {
      std::string text = type.is_soa ? "soa [" : "[";
      text.append(std::to_string(type.length));
      text.push_back(']');
      text.append(Describe(interner, type.element));
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
        text.append(Describe(interner, element));
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
        text.append(Describe(interner, param));
      }
      text.push_back(')');
      if (type.element.is_valid() &&
          KindOf(type.element) != TypeKind::kVoid) {
        text.append(" -> ");
        text.append(Describe(interner, type.element));
      }
      return text;
    }

    case TypeKind::kPointer:
      return (type.is_mutable ? "*mut " : "*") +
             Describe(interner, type.element);
    case TypeKind::kReference:
      return (type.is_mutable ? "&mut " : "&") +
             Describe(interner, type.element);

    case TypeKind::kQuantity: {
      // The dimension is what the reader cares about; the carrier is only
      // mentioned when it is not the default.
      std::string text = DimensionsOf(id).Describe(
          interner, [this](DimensionId d) { return DimensionName(d); });
      if (type.element.is_valid() &&
          !(KindOf(type.element) == TypeKind::kFloat &&
            Get(type.element).float_width == FloatWidth::k32)) {
        text.push_back(' ');
        text.push_back('(');
        text.append(Describe(interner, type.element));
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
