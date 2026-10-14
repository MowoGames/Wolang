// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_INTERP_VALUE_H_
#define WOLANG_INTERP_VALUE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wolang/sema/type.h"

namespace wolang {
namespace interp {

// A runtime value. The interpreter is the reference semantics, so this is
// deliberately simple rather than compact: an integer is always 64 bits wide
// with the declared width remembered separately, so that wrapping is a
// decision taken in one place instead of falling out of the storage.
class Value {
 public:
  enum class Kind : uint8_t {
    // A value that was never produced. Reading one is a trap, not a zero:
    // the IR only has an `undef` where control cannot reach.
    kUndefined,
    kVoid,
    kInt,
    kFloat,
    kBool,
    kString,
    // A struct, a tuple or an array: its elements, in order.
    kAggregate,
    // An optional: present with a value, or absent.
    kOptional,
    // A slot produced by `alloca`, identified by index into the frame's
    // storage.
    kSlot,
    // A function as a value: which function, in which module. The module
    // matters because lowering is per file, so the same index means
    // different functions in two of them.
    kFunction,
  };

  Value() = default;

  static Value Void() {
    Value value;
    value.kind_ = Kind::kVoid;
    return value;
  }
  static Value Int(uint64_t bits, IntWidth width, bool is_signed);
  static Value Float(double number, FloatWidth width);
  static Value Bool(bool flag);
  static Value Str(std::string text);
  static Value Aggregate(std::vector<Value> elements);
  static Value Present(Value inside);
  static Value Absent();
  static Value Slot(uint32_t index);
  static Value Function(uint32_t index, const void* module);

  Kind kind() const { return kind_; }
  bool is_undefined() const { return kind_ == Kind::kUndefined; }

  // The raw bits, already masked to the value's width.
  uint64_t integer() const { return integer_; }
  // Sign extended to 64 bits, for a signed comparison or a division.
  int64_t signed_integer() const;
  double floating() const { return floating_; }
  bool boolean() const { return boolean_; }
  const std::string& text() const { return *text_; }
  const std::vector<Value>& elements() const { return *elements_; }
  uint32_t slot() const { return integer_ == 0 ? 0 : integer_ - 1; }
  uint32_t function() const { return static_cast<uint32_t>(integer_); }
  const void* module() const { return module_; }

  IntWidth int_width() const { return int_width_; }
  bool is_signed() const { return is_signed_; }
  FloatWidth float_width() const { return float_width_; }

  bool has_value() const { return boolean_; }
  const Value& inside() const { return (*elements_)[0]; }

  // How a value reads in a trap message or a test expectation.
  std::string Describe() const;

  // Structural equality, which is what `eq` on an aggregate means.
  friend bool operator==(const Value& lhs, const Value& rhs);
  friend bool operator!=(const Value& lhs, const Value& rhs) {
    return !(lhs == rhs);
  }

 private:
  Kind kind_ = Kind::kUndefined;
  IntWidth int_width_ = IntWidth::k32;
  bool is_signed_ = true;
  FloatWidth float_width_ = FloatWidth::k32;
  bool boolean_ = false;
  uint64_t integer_ = 0;
  double floating_ = 0.0;
  // Shared so that copying a value is cheap; nothing mutates them in place.
  std::shared_ptr<const std::string> text_;
  std::shared_ptr<const std::vector<Value>> elements_;
  // Which module a function value belongs to. Not owned: a module outlives
  // every run over it.
  const void* module_ = nullptr;
};

// Masks |bits| to |width|, which is where wrapping happens and the only
// place it does.
uint64_t MaskToWidth(uint64_t bits, IntWidth width);

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_VALUE_H_
