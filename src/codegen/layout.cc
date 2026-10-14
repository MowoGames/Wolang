// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/layout.h"

#include <algorithm>

namespace wolang {
namespace codegen {

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
      return ValueClass::kInteger;
    case TypeKind::kFloat:
      return ValueClass::kFloat;
    case TypeKind::kVoid:
      return ValueClass::kVoid;
    case TypeKind::kOptional:
    case TypeKind::kTuple:
    case TypeKind::kArray:
      return ValueClass::kAggregate;
    case TypeKind::kNamed:
      // A class is a handle to a heap object and there is no heap yet; a
      // struct, a component or an enum stores its members inline.
      return resolution_.IsValueType(type)
                 ? ValueClass::kAggregate
                 : ValueClass::kUnsupported;
    default:
      // A string, a slice, a function, a `T!`, an unresolved generic: each
      // wants machinery a later wave brings.
      return ValueClass::kUnsupported;
  }
}

bool LayoutStore::IsScalar(TypeId type) const {
  const ValueClass value_class = ClassOf(type);
  return value_class == ValueClass::kInteger ||
         value_class == ValueClass::kFloat;
}

bool LayoutStore::IsTagOnly(TypeId type) const {
  if (types_.KindOf(type) != TypeKind::kNamed) {
    return false;
  }
  bool has_variant = false;
  for (const Member& member : resolution_.MembersOf(type)) {
    if (member.kind == MemberKind::kField) {
      return false;
    }
    has_variant = has_variant || member.kind == MemberKind::kVariant;
  }
  return has_variant;
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
  if (kind == TypeKind::kArray || kind == TypeKind::kOptional) {
    const TypeId element = types_.ElementOf(type);
    return element.is_valid() && IsSupported(element);
  }
  if (IsTagOnly(type)) {
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

uint32_t LayoutStore::FieldCount(TypeId type) const {
  switch (types_.KindOf(type)) {
    case TypeKind::kOptional:
      // A tag and the value it may hold.
      return 2;
    case TypeKind::kArray:
      return static_cast<uint32_t>(types_.Get(type).length);
    case TypeKind::kTuple:
      return static_cast<uint32_t>(types_.ArgumentsOf(type).size());
    case TypeKind::kNamed: {
      if (IsTagOnly(type)) {
        return 1;
      }
      uint32_t count = 0;
      for (const Member& member : resolution_.MembersOf(type)) {
        if (member.kind == MemberKind::kField) {
          ++count;
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
    case TypeKind::kArray:
      return types_.ElementOf(type);
    case TypeKind::kTuple: {
      const std::vector<TypeId> elements = types_.ArgumentsOf(type);
      return index < elements.size() ? elements[index] : TypeId{};
    }
    case TypeKind::kNamed: {
      uint32_t seen = 0;
      for (const Member& member : resolution_.MembersOf(type)) {
        if (member.kind != MemberKind::kField) {
          continue;
        }
        if (seen == index) {
          return member.type;
        }
        ++seen;
      }
      return TypeId{};
    }
    default:
      return TypeId{};
  }
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

  // An enum is the tag lowering reads as field zero.
  if (IsTagOnly(type)) {
    layout.size = 4;
    layout.align = 4;
    layout.offsets.push_back(0);
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

}  // namespace codegen
}  // namespace wolang
