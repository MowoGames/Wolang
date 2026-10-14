// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/abi/layout.h"

#include <algorithm>

namespace wolang {
namespace abi {

uint32_t AlignUp(uint32_t value, uint32_t align) {
  if (align <= 1) {
    return value;
  }
  return (value + align - 1) / align * align;
}

ValueClass LayoutStore::ClassOf(TypeId type) const {
  switch (types_.KindOf(type)) {
    case TypeKind::kBool:
    case TypeKind::kChar:
    case TypeKind::kInt:
    // A pointer is an integer that happens to be an address. Nothing in
    // this tier dereferences one -- only the foreign side does -- so it
    // needs no more than that.
    case TypeKind::kPointer:
    // A reference is an address too. The difference between it and a raw
    // pointer is what the language promises about it (D8), and a promise
    // has no representation.
    case TypeKind::kReference:
    // An error set is which error it is, and that is a number.
    case TypeKind::kErrorSet:
      return ValueClass::kInteger;
    case TypeKind::kFloat:
      return ValueClass::kFloat;
    case TypeKind::kVoid:
      return ValueClass::kVoid;
    case TypeKind::kOptional:
    case TypeKind::kFallible:
    case TypeKind::kTuple:
    case TypeKind::kArray:
    // A borrowed string is a pointer and a length, which is an aggregate
    // like any other. So is a slice -- a view of elements that are
    // somewhere else, which is what makes `[]u8` the buffer type and what
    // makes an array convert to one.
    case TypeKind::kStr:
    case TypeKind::kSlice:
      return ValueClass::kAggregate;
    // An instance of a generic type is laid out like the declaration it
    // came from, with its members' types bound -- which the member table
    // of an instance already does.
    case TypeKind::kInstance:
    case TypeKind::kNamed:
      // A struct, a component or an enum stores its members inline. A
      // class is a handle to a heap object, so a value of one is an
      // address -- which is why it travels in a register and why a method
      // of a class mutates the object it was called on. A trait value is
      // a pair, so it is an aggregate whatever implements it.
      if (resolution_.IsTrait(type)) {
        return ValueClass::kAggregate;
      }
      return types_.IsHandle(type) ? ValueClass::kInteger
                                  : ValueClass::kAggregate;
    case TypeKind::kFunction:
      // A function as a value is its address.
      return ValueClass::kInteger;
    default:
      // A string, a slice, a `T!`, an unresolved generic: each wants
      // machinery a later wave brings.
      return ValueClass::kUnsupported;
  }
}

bool LayoutStore::IsScalar(TypeId type) const {
  const ValueClass value_class = ClassOf(type);
  return value_class == ValueClass::kInteger ||
         value_class == ValueClass::kFloat;
}

bool LayoutStore::IsTagOnly(TypeId type) const {
  const TypeKind kind = types_.KindOf(type);
  if (kind != TypeKind::kNamed && kind != TypeKind::kInstance) {
    return false;
  }
  bool has_variant = false;
  for (const Member& member : resolution_.MembersOf(type)) {
    if (HasStorage(member.kind)) {
      return false;
    }
    has_variant = has_variant || member.kind == MemberKind::kVariant;
  }
  // A variant that carries something makes the enum a tagged union
  // instead: the tag says which one, and the payload sits beside it.
  return has_variant && !IsTagged(type);
}

bool LayoutStore::IsTagged(TypeId type) const {
  const TypeKind kind = types_.KindOf(type);
  if (kind != TypeKind::kNamed && kind != TypeKind::kInstance) {
    return false;
  }
  for (const Member& member : resolution_.MembersOf(type)) {
    // A variant that carries values has a function type: the values it
    // takes, and the enum it produces.
    if (member.kind == MemberKind::kVariant &&
        types_.KindOf(member.type) == TypeKind::kFunction) {
      return true;
    }
  }
  return false;
}

// How much room one variant's values need, and how aligned they must be.
// Walked field by field rather than through a tuple type, because working
// that out would mean interning one -- and a layout answers questions
// about types rather than making them.
void LayoutStore::MeasurePayload(TypeId variant,
                                 uint32_t* size,
                                 uint32_t* align) {
  *size = 0;
  *align = 1;
  for (TypeId field : types_.ArgumentsOf(variant)) {
    if (!field.is_valid()) {
      continue;
    }
    const Layout inner = Of(field);
    *align = std::max(*align, inner.align);
    *size = AlignUp(*size, inner.align) + inner.size;
  }
  *size = AlignUp(*size, *align);
}

TypeId LayoutStore::VariantFieldType(TypeId type,
                                     uint32_t index,
                                     uint32_t position) const {
  uint32_t seen = 0;
  for (const Member& member : resolution_.MembersOf(type)) {
    if (member.kind != MemberKind::kVariant) {
      continue;
    }
    if (seen++ != index) {
      continue;
    }
    if (types_.KindOf(member.type) != TypeKind::kFunction) {
      return TypeId{};
    }
    const std::vector<TypeId> fields = types_.ArgumentsOf(member.type);
    return position < fields.size() ? fields[position] : TypeId{};
  }
  return TypeId{};
}

uint32_t LayoutStore::VariantFieldOffset(TypeId type,
                                         uint32_t index,
                                         uint32_t position) {
  uint32_t offset = PayloadOffsetOf(type);
  for (uint32_t i = 0;; ++i) {
    const TypeId field = VariantFieldType(type, index, i);
    if (!field.is_valid()) {
      break;
    }
    const Layout inner = Of(field);
    offset = AlignUp(offset, inner.align);
    if (i == position) {
      break;
    }
    offset += inner.size;
  }
  return offset;
}

uint32_t LayoutStore::PayloadOffsetOf(TypeId type) {
  uint32_t align = 4;
  for (const Member& member : resolution_.MembersOf(type)) {
    if (member.kind != MemberKind::kVariant ||
        types_.KindOf(member.type) != TypeKind::kFunction) {
      continue;
    }
    uint32_t size = 0;
    uint32_t inner = 1;
    MeasurePayload(member.type, &size, &inner);
    align = std::max(align, inner);
  }
  return AlignUp(4, align);
}

uint32_t LayoutStore::PayloadSizeOf(TypeId type) {
  uint32_t largest = 0;
  for (const Member& member : resolution_.MembersOf(type)) {
    if (member.kind != MemberKind::kVariant ||
        types_.KindOf(member.type) != TypeKind::kFunction) {
      continue;
    }
    uint32_t size = 0;
    uint32_t align = 1;
    MeasurePayload(member.type, &size, &align);
    largest = std::max(largest, size);
  }
  return largest;
}

bool LayoutStore::IsSupported(TypeId type) const {
  switch (ClassOf(type)) {
    case ValueClass::kUnsupported:
      return false;
    case ValueClass::kInteger:
    case ValueClass::kFloat:
    case ValueClass::kVoid:
      return true;
    case ValueClass::kAggregate:
      break;
  }
  const TypeKind kind = types_.KindOf(type);
  if (kind == TypeKind::kStr) {
    return true;
  }
  if (kind == TypeKind::kSlice) {
    const TypeId element = types_.ElementOf(type);
    return element.is_valid() && IsSupported(element);
  }
  if (kind == TypeKind::kArray || kind == TypeKind::kOptional ||
      kind == TypeKind::kFallible) {
    const TypeId element = types_.ElementOf(type);
    return element.is_valid() && IsSupported(element);
  }
  if (IsTagOnly(type)) {
    return true;
  }
  // A tagged union is supported when everything its variants carry is:
  // the payload's type is per variant, so the generic field walk below
  // cannot see it (D89).
  if (IsTagged(type)) {
    uint32_t index = 0;
    for (const Member& member : resolution_.MembersOf(type)) {
      if (member.kind != MemberKind::kVariant) {
        continue;
      }
      for (uint32_t position = 0;; ++position) {
        const TypeId carried = VariantFieldType(type, index, position);
        if (!carried.is_valid()) {
          break;
        }
        if (!IsSupported(carried)) {
          return false;
        }
      }
      ++index;
    }
    return true;
  }
  const uint32_t fields = FieldCount(type);
  if (fields == 0) {
    // A struct with no fields has nothing to hold, which is fine.
    return true;
  }
  for (uint32_t i = 0; i < fields; ++i) {
    const TypeId field = FieldType(type, i);
    if (!field.is_valid() || !IsSupported(field)) {
      return false;
    }
  }
  return true;
}

bool LayoutStore::HasHeader(TypeId type) const {
  if (!types_.IsHandle(type)) {
    return false;
  }
  TypeId current = type;
  while (current.is_valid()) {
    for (const Member& member : resolution_.MembersOf(current)) {
      if (member.kind == MemberKind::kMethod &&
          (member.is_virtual || member.is_override)) {
        return true;
      }
    }
    current = resolution_.BaseOf(current);
  }
  return false;
}

uint32_t LayoutStore::InheritedFieldCount(TypeId type) const {
  uint32_t count = 0;
  TypeId current = resolution_.BaseOf(type);
  while (current.is_valid()) {
    for (const Member& member : resolution_.MembersOf(current)) {
      if (HasStorage(member.kind)) {
        ++count;
      }
    }
    current = resolution_.BaseOf(current);
  }
  return count;
}

// The classes |type| derives from, root first.
namespace {

template <typename Resolution>
std::vector<TypeId> Chain(const Resolution& resolution, TypeId type) {
  std::vector<TypeId> chain;
  for (TypeId current = type; current.is_valid();
       current = resolution.BaseOf(current)) {
    chain.push_back(current);
  }
  std::reverse(chain.begin(), chain.end());
  return chain;
}

}  // namespace

uint32_t LayoutStore::FieldCount(TypeId type) const {
  switch (types_.KindOf(type)) {
    case TypeKind::kOptional:
    // A `T!` is the same shape: which it is, and the value when it is a
    // value (D90).
    case TypeKind::kFallible:
      // A tag and the value it may hold.
      return 2;
    case TypeKind::kArray:
      return static_cast<uint32_t>(types_.Get(type).length);
    case TypeKind::kTuple:
      return static_cast<uint32_t>(types_.ArgumentsOf(type).size());
    case TypeKind::kStr:
    case TypeKind::kSlice:
      // The elements and how many there are.
      return 2;
    case TypeKind::kInstance:
    case TypeKind::kNamed: {
      if (resolution_.IsTrait(type)) {
        return 2;
      }
      if (IsTagOnly(type)) {
        return 1;
      }
      if (IsTagged(type)) {
        // The tag and the payload beside it. The payload's *type* is the
        // instruction's rather than the declaration's, because which
        // variant it is is what a `match` is for.
        return 2;
      }
      // A class's fields are its base's fields and then its own, with the
      // descriptor first when anything in the chain is virtual.
      uint32_t count = HasHeader(type) ? 1u : 0u;
      for (TypeId current : Chain(resolution_, type)) {
        for (const Member& member : resolution_.MembersOf(current)) {
          if (HasStorage(member.kind)) {
            ++count;
          }
        }
      }
      return count;
    }
    default:
      return 0;
  }
}

TypeId LayoutStore::FieldType(TypeId type, uint32_t index) const {
  switch (types_.KindOf(type)) {
    case TypeKind::kOptional:
      return index == 0 ? types_.bool_type() : types_.ElementOf(type);
    case TypeKind::kFallible:
      // Which error, or zero for a value. A number rather than a bool,
      // because a `T!` says *which* way it failed.
      return index == 0 ? types_.u32() : types_.ElementOf(type);
    case TypeKind::kArray:
      return types_.ElementOf(type);
    case TypeKind::kTuple: {
      const std::vector<TypeId> elements = types_.ArgumentsOf(type);
      return index < elements.size() ? elements[index] : TypeId{};
    }
    case TypeKind::kInstance:
    case TypeKind::kNamed: {
      if (resolution_.IsTrait(type)) {
        // What it points at, and which class that is.
        return types_.u64();
      }
      if (IsTagged(type)) {
        return index == 0 ? types_.u32() : TypeId{};
      }
      uint32_t seen = 0;
      // The descriptor, when there is one: a number saying which class
      // this object really is. Its type is not declared by any member, so
      // it is spelled here.
      if (HasHeader(type)) {
        if (index == 0) {
          return types_.u64();
        }
        seen = 1;
      }
      for (TypeId current : Chain(resolution_, type)) {
        for (const Member& member : resolution_.MembersOf(current)) {
          if (!HasStorage(member.kind)) {
            continue;
          }
          if (seen == index) {
            return member.type;
          }
          ++seen;
        }
      }
      return TypeId{};
    }
    default:
      return TypeId{};
  }
}

uint64_t LayoutStore::SoaPrefixOf(TypeId record, uint32_t field) {
  uint64_t prefix = 0;
  const uint32_t fields = FieldCount(record);
  for (uint32_t i = 0; i < field && i < fields; ++i) {
    const TypeId type = FieldType(record, i);
    if (type.is_valid()) {
      prefix += Of(type).size;
    }
  }
  return prefix;
}

uint64_t LayoutStore::SoaOffsetOf(TypeId record,
                                  uint32_t field,
                                  uint64_t index,
                                  uint64_t count) {
  const TypeId type = FieldType(record, field);
  const uint64_t size = type.is_valid() ? Of(type).size : 0;
  return SoaPrefixOf(record, field) * SoaRoundCount(count) + index * size;
}

uint64_t LayoutStore::SoaSizeOf(TypeId record, uint64_t count) {
  return SoaPrefixOf(record, FieldCount(record)) * SoaRoundCount(count);
}

uint32_t LayoutStore::StrideOf(TypeId array) {
  const TypeId element = types_.ElementOf(array);
  if (!element.is_valid()) {
    return 0;
  }
  const Layout& inner = Of(element);
  return AlignUp(inner.size, inner.align);
}

const Layout& LayoutStore::Of(TypeId type) {
  const auto found = cache_.find(type.index);
  if (found != cache_.end()) {
    return found->second;
  }
  // Inserted before the recursive walk so that a type the resolution let
  // refer to itself cannot loop forever here; `E0117` is what actually
  // rejects such a type.
  cache_.emplace(type.index, Layout{});
  Layout layout = Compute(type);
  cache_[type.index] = std::move(layout);
  return cache_[type.index];
}

bool LayoutStore::IsHandle(TypeId type) const {
  return types_.IsHandle(type);
}

const Layout& LayoutStore::ObjectOf(TypeId type) {
  if (!IsHandle(type)) {
    return Of(type);
  }
  const auto found = objects_.find(type.index);
  if (found != objects_.end()) {
    return found->second;
  }
  // The same field walk a struct gets. Inserted before it for the same
  // reason `Of` does: a class may contain itself, and that is the whole
  // point of being a handle.
  objects_.emplace(type.index, Layout{});
  objects_[type.index] = ComputeFields(type);
  return objects_[type.index];
}

Layout LayoutStore::Compute(TypeId type) {
  Layout layout;
  switch (ClassOf(type)) {
    case ValueClass::kVoid:
    case ValueClass::kUnsupported:
      return layout;

    case ValueClass::kInteger:
    case ValueClass::kFloat: {
      const uint32_t bits = BitsOf(type);
      layout.size = std::max(1u, bits / 8);
      layout.align = layout.size;
      return layout;
    }

    case ValueClass::kAggregate:
      break;
  }

  // A trait value: what it points at, and which class that is. Two words
  // for the same reason a slice is two -- neither half is a member anyone
  // declared, and a call on it needs both (D86).
  if (resolution_.IsTrait(type)) {
    layout.size = 16;
    layout.align = 8;
    layout.offsets.push_back(0);
    layout.offsets.push_back(8);
    return layout;
  }

  // A borrowed string: the bytes and how many there are. Written out here
  // rather than through the field walk, because neither field is a member
  // anyone declared -- `str` is built in.
  if (types_.KindOf(type) == TypeKind::kStr ||
      types_.KindOf(type) == TypeKind::kSlice) {
    layout.size = 16;
    layout.align = 8;
    layout.offsets.push_back(0);
    layout.offsets.push_back(8);
    return layout;
  }

  // An enum is the tag lowering reads as field zero.
  if (IsTagOnly(type)) {
    layout.size = 4;
    layout.align = 4;
    layout.offsets.push_back(0);
    return layout;
  }

  // A tagged union: the tag, then room for the largest payload (D89).
  if (IsTagged(type)) {
    const uint32_t offset = PayloadOffsetOf(type);
    const uint32_t payload = PayloadSizeOf(type);
    layout.align = std::max(4u, offset);
    layout.size = AlignUp(offset + payload, layout.align);
    layout.offsets.push_back(0);
    layout.offsets.push_back(offset);
    return layout;
  }

  // An array is described by its stride rather than by an offset each, or a
  // large one would cost a vector entry per element.
  if (types_.KindOf(type) == TypeKind::kArray) {
    const TypeId element = types_.ElementOf(type);
    if (element.is_valid()) {
      const Layout inner = Of(element);
      const uint32_t stride = AlignUp(inner.size, inner.align);
      layout.align = inner.align;
      layout.size = stride * static_cast<uint32_t>(types_.Get(type).length);
    }
    return layout;
  }

  return ComputeFields(type);
}

// Members laid out in declaration order, each at the next offset its
// alignment allows.
Layout LayoutStore::ComputeFields(TypeId type) {
  Layout layout;
  const uint32_t fields = FieldCount(type);
  uint32_t offset = 0;
  uint32_t align = 1;
  for (uint32_t i = 0; i < fields; ++i) {
    const TypeId field = FieldType(type, i);
    if (!field.is_valid()) {
      continue;
    }
    const Layout inner = Of(field);
    align = std::max(align, inner.align);
    offset = AlignUp(offset, inner.align);
    layout.offsets.push_back(offset);
    offset += inner.size;
  }
  layout.align = align;
  // The trailing padding is part of the size, or an array of these would
  // put its second element at the wrong offset.
  layout.size = AlignUp(offset, align);
  return layout;
}

uint32_t LayoutStore::BitsOf(TypeId type) const {
  switch (types_.KindOf(type)) {
    case TypeKind::kBool:
      return 8;
    case TypeKind::kChar:
      return 32;
    case TypeKind::kPointer:
    case TypeKind::kReference:
      return 64;
    case TypeKind::kErrorSet:
      return 32;
    case TypeKind::kInstance:
    case TypeKind::kNamed:
    case TypeKind::kFunction:
      // A class and a function are both addresses; a value type never
      // reaches here, because its class is `kAggregate`.
      return 64;
    case TypeKind::kInt:
      switch (types_.Get(type).int_width) {
        case IntWidth::k8:
          return 8;
        case IntWidth::k16:
          return 16;
        case IntWidth::k32:
          return 32;
        case IntWidth::k64:
          return 64;
      }
      return 32;
    case TypeKind::kFloat:
      return types_.Get(type).float_width == FloatWidth::k64 ? 64 : 32;
    default:
      return 0;
  }
}

bool LayoutStore::IsSignedInteger(TypeId type) const {
  const Type& described = types_.Get(type);
  if (described.kind == TypeKind::kInt) {
    return described.is_signed;
  }
  // A `bool` and a `char` are unsigned; neither is ever negative.
  return false;
}

}  // namespace abi
}  // namespace wolang
