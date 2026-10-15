// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

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
// One step of a place: a field of an aggregate, or an element of an array.
//
// The two behave differently when another element step follows. A field
// goes inwards; an element *adds*, because a pointer to element three plus
// two is a pointer to element five and not an element of an element. The
// compiled tiers get that for free from arithmetic; here it has to be
// written down.
struct PlaceStep {
  uint32_t index = 0;
  bool is_element = false;

  friend bool operator==(const PlaceStep& lhs, const PlaceStep& rhs) {
    return lhs.index == rhs.index && lhs.is_element == rhs.is_element;
  }
};

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
    // Somewhere inside a slot: which frame, which slot, and the steps to
    // walk from it.
    //
    // The compiled tiers have addresses for this; the interpreter's values
    // carry their shape instead, so a place is a path rather than a
    // number. It names its frame because it may be passed to a callee --
    // which is what a slice of a local array is -- and the frame is safe
    // to hold on to because a reference cannot outlive it (`E0121`).
    kPlace,
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
  // |type| is what is in the slot, carried along because a value does not
  // otherwise know its own type and the one place that needs it -- handing
  // a buffer to a C function -- has nothing else to ask. A place keeps the
  // *slot's* type rather than the leaf's: a C function given `&buf[2]`
  // writes wherever in `buf` it likes, so the whole slot is what has to be
  // copied out and back.
  static Value Slot(uint32_t index, TypeId type = TypeId{});
  static Value Function(uint32_t index, const void* module);
  static Value Place(const void* frame,
                     uint32_t slot,
                     std::vector<PlaceStep> path,
                     TypeId type = TypeId{});

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
  const std::vector<PlaceStep>& path() const { return *path_; }
  // What the slot at the root of this place holds.
  TypeId place_type() const { return place_type_; }

  // Element |index|, ready to be written to. The elements are shared so
  // that copying a value is cheap, so writing into one needs a copy of its
  // own first -- which is what keeps two copies of an aggregate two
  // values rather than one.
  Value* ElementForWriting(uint32_t index);
  const void* module() const { return module_; }
  // Which frame a place belongs to.
  const void* frame() const { return module_; }

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
  // Which module a function value belongs to, or which frame a place is
  // in. Not owned: a module outlives every run over it, and a reference
  // cannot outlive the frame it points into.
  const void* module_ = nullptr;
  // The steps of a place, shared so that extending one is cheap.
  std::shared_ptr<const std::vector<PlaceStep>> path_;
  TypeId place_type_;
};

// Masks |bits| to |width|, which is where wrapping happens and the only
// place it does.
uint64_t MaskToWidth(uint64_t bits, IntWidth width);

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_VALUE_H_
