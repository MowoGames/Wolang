// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/interp/value.h"

#include <cstdio>

namespace wolang {
namespace interp {
namespace {

uint32_t BitsOf(IntWidth width) {
  switch (width) {
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
}

}  // namespace

uint64_t MaskToWidth(uint64_t bits, IntWidth width) {
  const uint32_t count = BitsOf(width);
  if (count >= 64) {
    return bits;
  }
  return bits & ((uint64_t{1} << count) - 1);
}

Value Value::Int(uint64_t bits, IntWidth width, bool is_signed) {
  Value value;
  value.kind_ = Kind::kInt;
  value.int_width_ = width;
  value.is_signed_ = is_signed;
  value.integer_ = MaskToWidth(bits, width);
  return value;
}

Value Value::Float(double number, FloatWidth width) {
  Value value;
  value.kind_ = Kind::kFloat;
  value.float_width_ = width;
  // An f32 has to round through a float, or arithmetic would quietly be done
  // at double precision and the interpreter would disagree with every other
  // tier.
  value.floating_ = width == FloatWidth::k32
                        ? static_cast<double>(static_cast<float>(number))
                        : number;
  return value;
}

Value Value::Bool(bool flag) {
  Value value;
  value.kind_ = Kind::kBool;
  value.boolean_ = flag;
  return value;
}

Value Value::Str(std::string text) {
  Value value;
  value.kind_ = Kind::kString;
  value.text_ = std::make_shared<const std::string>(std::move(text));
  return value;
}

Value Value::Aggregate(std::vector<Value> elements) {
  Value value;
  value.kind_ = Kind::kAggregate;
  value.elements_ =
      std::make_shared<const std::vector<Value>>(std::move(elements));
  return value;
}

Value Value::Present(Value inside) {
  Value value;
  value.kind_ = Kind::kOptional;
  value.boolean_ = true;
  value.elements_ = std::make_shared<const std::vector<Value>>(
      std::vector<Value>{std::move(inside)});
  return value;
}

Value Value::Absent() {
  Value value;
  value.kind_ = Kind::kOptional;
  value.boolean_ = false;
  value.elements_ = std::make_shared<const std::vector<Value>>();
  return value;
}

Value Value::Slot(uint32_t index, TypeId type) {
  Value value;
  value.kind_ = Kind::kSlot;
  value.place_type_ = type;
  // Offset by one so that a zeroed value is not a valid slot.
  value.integer_ = uint64_t{index} + 1;
  return value;
}

Value Value::Function(uint32_t index, const void* module) {
  Value value;
  value.kind_ = Kind::kFunction;
  value.integer_ = index;
  value.module_ = module;
  return value;
}

Value Value::Place(const void* frame,
                   uint32_t slot,
                   std::vector<PlaceStep> path,
                   TypeId type) {
  Value value;
  value.kind_ = Kind::kPlace;
  value.integer_ = slot;
  value.module_ = frame;
  value.place_type_ = type;
  value.path_ =
      std::make_shared<const std::vector<PlaceStep>>(std::move(path));
  return value;
}

Value* Value::ElementForWriting(uint32_t index) {
  if (kind_ != Kind::kAggregate || elements_ == nullptr) {
    return nullptr;
  }
  if (index >= elements_->size()) {
    return nullptr;
  }
  if (elements_.use_count() > 1) {
    elements_ = std::make_shared<const std::vector<Value>>(*elements_);
  }
  // The storage is const so that a shared value cannot be changed through
  // another handle to it; this is the one place that writes, and it has
  // just made the storage its own.
  auto* elements = const_cast<std::vector<Value>*>(elements_.get());
  return &(*elements)[index];
}

int64_t Value::signed_integer() const {
  const uint32_t count = BitsOf(int_width_);
  if (count >= 64) {
    return static_cast<int64_t>(integer_);
  }
  const uint64_t sign = uint64_t{1} << (count - 1);
  if ((integer_ & sign) == 0) {
    return static_cast<int64_t>(integer_);
  }
  // Sign extend by filling the bits above the width.
  return static_cast<int64_t>(integer_ | ~((uint64_t{1} << count) - 1));
}

std::string Value::Describe() const {
  switch (kind_) {
    case Kind::kUndefined:
      return "undefined";
    case Kind::kVoid:
      return "()";
    case Kind::kInt:
      return is_signed_ ? std::to_string(signed_integer())
                        : std::to_string(integer_);
    case Kind::kFloat: {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%g", floating_);
      return buffer;
    }
    case Kind::kBool:
      return boolean_ ? "true" : "false";
    case Kind::kString:
      return "\"" + *text_ + "\"";
    case Kind::kAggregate: {
      std::string result = "(";
      for (size_t i = 0; i < elements_->size(); ++i) {
        if (i != 0) {
          result.append(", ");
        }
        result.append((*elements_)[i].Describe());
      }
      result.push_back(')');
      return result;
    }
    case Kind::kOptional:
      return boolean_ ? inside().Describe() : "null";
    case Kind::kSlot:
      return "slot " + std::to_string(slot());
    case Kind::kFunction:
      return "function " + std::to_string(function());
    case Kind::kPlace: {
      std::string text = "slot " + std::to_string(integer_);
      for (const PlaceStep& step : *path_) {
        text.append(step.is_element ? "[" : ".");
        text.append(std::to_string(step.index));
        if (step.is_element) {
          text.append("]");
        }
      }
      return text;
    }
  }
  return "?";
}

bool operator==(const Value& lhs, const Value& rhs) {
  if (lhs.kind_ != rhs.kind_) {
    return false;
  }
  switch (lhs.kind_) {
    case Value::Kind::kUndefined:
    case Value::Kind::kVoid:
      return true;
    case Value::Kind::kInt:
      return lhs.integer_ == rhs.integer_;
    case Value::Kind::kFloat:
      return lhs.floating_ == rhs.floating_;
    case Value::Kind::kBool:
      return lhs.boolean_ == rhs.boolean_;
    case Value::Kind::kString:
      return *lhs.text_ == *rhs.text_;
    case Value::Kind::kSlot:
      return lhs.integer_ == rhs.integer_;
    case Value::Kind::kFunction:
      // The same function of the same module, which is what makes two
      // handlers comparable.
      return lhs.integer_ == rhs.integer_ && lhs.module_ == rhs.module_;
    case Value::Kind::kPlace:
      return lhs.module_ == rhs.module_ &&
             lhs.integer_ == rhs.integer_ && *lhs.path_ == *rhs.path_;
    case Value::Kind::kAggregate:
      return *lhs.elements_ == *rhs.elements_;
    case Value::Kind::kOptional:
      if (lhs.boolean_ != rhs.boolean_) {
        return false;
      }
      return !lhs.boolean_ || lhs.inside() == rhs.inside();
  }
  return false;
}

}  // namespace interp
}  // namespace wolang
