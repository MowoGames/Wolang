// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/interp/marshal.h"

#include <cstring>
#include <vector>

namespace wolang {
namespace interp {

uint64_t CanonicalBits(abi::LayoutStore* layouts,
                       const interp::Value& value,
                       TypeId type) {
  const uint32_t bits = layouts->BitsOf(type);
  if (value.kind() == interp::Value::Kind::kBool) {
    return value.boolean() ? 1 : 0;
  }
  if (bits >= 64 || bits == 0) {
    return value.integer();
  }
  const uint64_t mask = (uint64_t{1} << bits) - 1;
  uint64_t raw = value.integer() & mask;
  if (layouts->IsSignedInteger(type) &&
      (raw & (uint64_t{1} << (bits - 1))) != 0) {
    raw |= ~mask;
  }
  return raw;
}

void Encode(const TypeStore& types,
            abi::LayoutStore* layouts,
            const interp::Value& value,
            TypeId type,
            uint8_t* destination) {
  switch (layouts->ClassOf(type)) {
    case abi::ValueClass::kVoid:
    case abi::ValueClass::kUnsupported:
      return;

    case abi::ValueClass::kInteger: {
      const uint64_t raw = CanonicalBits(layouts, value, type);
      const uint32_t size = layouts->SizeOf(type);
      std::memcpy(destination, &raw, size);
      return;
    }

    case abi::ValueClass::kFloat: {
      if (layouts->BitsOf(type) == 64) {
        const double number = value.floating();
        std::memcpy(destination, &number, sizeof(number));
      } else {
        const float number = static_cast<float>(value.floating());
        std::memcpy(destination, &number, sizeof(number));
      }
      return;
    }

    case abi::ValueClass::kAggregate:
      break;
  }

  const abi::Layout& layout = layouts->Of(type);
  if (types.KindOf(type) == TypeKind::kStr ||
      types.KindOf(type) == TypeKind::kString) {
    // The pair the value already is. Both tiers hold a string the same
    // way, so crossing the boundary copies two words and nothing else --
    // which is also why a string survives the crossing at all.
    uint64_t address = 0;
    uint64_t length = 0;
    if (value.kind() == interp::Value::Kind::kAggregate &&
        value.elements().size() == 2) {
      address = value.elements()[0].integer();
      length = value.elements()[1].integer();
    }
    std::memcpy(destination, &address, sizeof(address));
    std::memcpy(destination + 8, &length, sizeof(length));
    return;
  }
  if (types.KindOf(type) == TypeKind::kOptional) {
    const uint8_t tag = value.kind() == interp::Value::Kind::kOptional &&
                                value.has_value()
                            ? 1
                            : 0;
    destination[0] = tag;
    if (tag != 0 && layout.offsets.size() > 1) {
      Encode(types, layouts, value.inside(), types.ElementOf(type),
             destination + layout.offsets[1]);
    }
    return;
  }

  if (value.kind() != interp::Value::Kind::kAggregate) {
    return;
  }
  const std::vector<interp::Value>& elements = value.elements();
  if (types.KindOf(type) == TypeKind::kArray) {
    const TypeId element = types.ElementOf(type);
    const uint32_t stride = layouts->StrideOf(type);
    for (size_t i = 0; i < elements.size(); ++i) {
      Encode(types, layouts, elements[i], element, destination + i * stride);
    }
    return;
  }
  if (layouts->IsTagOnly(type)) {
    if (!elements.empty()) {
      const uint32_t tag = static_cast<uint32_t>(elements[0].integer());
      std::memcpy(destination, &tag, sizeof(tag));
    }
    return;
  }
  for (size_t i = 0; i < elements.size() && i < layout.offsets.size(); ++i) {
    const TypeId field = layouts->FieldType(type, static_cast<uint32_t>(i));
    if (field.is_valid()) {
      Encode(types, layouts, elements[i], field, destination + layout.offsets[i]);
    }
  }
}

interp::Value Decode(const TypeStore& types,
                     abi::LayoutStore* layouts,
                     const uint8_t* source,
                     TypeId type) {
  switch (layouts->ClassOf(type)) {
    case abi::ValueClass::kVoid:
      return interp::Value::Void();
    case abi::ValueClass::kUnsupported:
      return interp::Value();

    case abi::ValueClass::kInteger: {
      uint64_t raw = 0;
      std::memcpy(&raw, source, layouts->SizeOf(type));
      if (types.KindOf(type) == TypeKind::kBool) {
        return interp::Value::Bool((raw & 1) != 0);
      }
      const Type& described = types.Get(type);
      return interp::Value::Int(raw, described.int_width,
                                described.is_signed);
    }

    case abi::ValueClass::kFloat: {
      if (layouts->BitsOf(type) == 64) {
        double number = 0;
        std::memcpy(&number, source, sizeof(number));
        return interp::Value::Float(number, FloatWidth::k64);
      }
      float number = 0;
      std::memcpy(&number, source, sizeof(number));
      return interp::Value::Float(static_cast<double>(number),
                                  FloatWidth::k32);
    }

    case abi::ValueClass::kAggregate:
      break;
  }

  const abi::Layout& layout = layouts->Of(type);
  if (types.KindOf(type) == TypeKind::kStr ||
      types.KindOf(type) == TypeKind::kString) {
    // The pair, not a copy of the bytes: a string is a pointer and a
    // length in every tier, and handing one back as something else is how
    // the two tiers used to be unable to pass a string to each other.
    uint64_t address = 0;
    uint64_t length = 0;
    std::memcpy(&address, source, sizeof(address));
    std::memcpy(&length, source + 8, sizeof(length));
    std::vector<interp::Value> pair;
    pair.push_back(
        interp::Value::Int(address, IntWidth::k64, /*is_signed=*/false));
    pair.push_back(
        interp::Value::Int(length, IntWidth::k64, /*is_signed=*/false));
    return interp::Value::Aggregate(std::move(pair));
  }
  if (types.KindOf(type) == TypeKind::kOptional) {
    if (source[0] == 0) {
      return interp::Value::Absent();
    }
    const TypeId inside = types.ElementOf(type);
    const uint32_t offset =
        layout.offsets.size() > 1 ? layout.offsets[1] : 0;
    return interp::Value::Present(
        Decode(types, layouts, source + offset, inside));
  }
  if (types.KindOf(type) == TypeKind::kArray) {
    const TypeId element = types.ElementOf(type);
    const uint32_t stride = layouts->StrideOf(type);
    std::vector<interp::Value> elements;
    for (uint64_t i = 0; i < types.Get(type).length; ++i) {
      elements.push_back(Decode(types, layouts, source + i * stride, element));
    }
    return interp::Value::Aggregate(std::move(elements));
  }
  if (layouts->IsTagOnly(type)) {
    uint32_t tag = 0;
    std::memcpy(&tag, source, sizeof(tag));
    std::vector<interp::Value> elements;
    elements.push_back(interp::Value::Int(tag, IntWidth::k32, true));
    return interp::Value::Aggregate(std::move(elements));
  }
  std::vector<interp::Value> elements;
  for (size_t i = 0; i < layout.offsets.size(); ++i) {
    const TypeId field = layouts->FieldType(type, static_cast<uint32_t>(i));
    if (!field.is_valid()) {
      continue;
    }
    elements.push_back(
        Decode(types, layouts, source + layout.offsets[i], field));
  }
  return interp::Value::Aggregate(std::move(elements));
}

// ------------------------------------------------------------ running ---

}  // namespace interp
}  // namespace wolang
