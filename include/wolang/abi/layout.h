// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_ABI_LAYOUT_H_
#define WOLANG_ABI_LAYOUT_H_

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "wolang/sema/resolution.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace abi {

// Where a value of some type lives.
//
// Its own layer rather than part of the code generator, because three
// things need the same answer: the emitter, the C boundary, and the
// interpreter whenever it has to read a value out of raw memory. A layout
// is about the machine, not about the type system, which is why it is not
// in `sema` either.
enum class ValueClass : uint8_t {
  // A general purpose register: `bool`, every integer width, a tag.
  kInteger,
  // A floating point register.
  kFloat,
  // Memory, addressed by a pointer. A struct, a tuple, an array and an
  // optional are all this: the dev tier keeps them in the frame rather than
  // scattering their fields across registers.
  kAggregate,
  // Nothing is stored.
  kVoid,
  // Something this tier cannot hold yet -- a string, a slice, a closure.
  // The dev tier leaves a function that mentions one to the interpreter
  // rather than guessing.
  kUnsupported,
};

struct Layout {
  uint32_t size = 0;
  uint32_t align = 1;
  // Where each field starts, in declaration order. A struct's fields, a
  // tuple's elements, or an optional's tag and payload.
  std::vector<uint32_t> offsets;
};

// Answers the layout questions for one file's types. Holds a reference to
// the resolution because a struct's fields are part of the file resolution
// that declared them, not of the type store.
class LayoutStore {
 public:
  LayoutStore(const TypeStore& types, const FileResolution& resolution)
      : types_(types), resolution_(resolution) {}

  ValueClass ClassOf(TypeId type) const;
  // True when a value of |type| is held in a register.
  bool IsScalar(TypeId type) const;
  // True when the dev tier can hold a value of this type at all.
  bool IsSupported(TypeId type) const;

  const Layout& Of(TypeId type);
  // The layout of what a class *points at*: its fields laid out the way a
  // struct's are. `Of` a class is the handle -- eight bytes -- so this is
  // the one an allocation and a field offset need.
  const Layout& ObjectOf(TypeId type);
  // True when a value of |type| is a handle to a heap object.
  bool IsHandle(TypeId type) const;
  uint32_t SizeOf(TypeId type) { return Of(type).size; }
  uint32_t AlignOf(TypeId type) { return Of(type).align; }
  // The type of field |index| of an aggregate, or an invalid id -- which
  // is what an enum's tag is: its type is the instruction's, not the
  // declaration's, because no member declares it.
  TypeId FieldType(TypeId type, uint32_t index) const;
  // How many fields an aggregate has.
  uint32_t FieldCount(TypeId type) const;
  // The distance between two elements of an array, which is the element's
  // size rounded up to its alignment.
  uint32_t StrideOf(TypeId array);
  // True for an enum: its members are variants, so the value is the tag
  // lowering reads as field zero and nothing else.
  bool IsTagOnly(TypeId type) const;

  // The width a scalar occupies in a register, which is what tells `ldr`
  // from `ldrb` and `fadd s` from `fadd d`.
  uint32_t BitsOf(TypeId type) const;
  bool IsSignedInteger(TypeId type) const;

 private:
  Layout Compute(TypeId type);
  Layout ComputeFields(TypeId type);

  const TypeStore& types_;
  const FileResolution& resolution_;
  std::unordered_map<uint32_t, Layout> cache_;
  // The storage a class points at, kept apart from the handle's layout.
  std::unordered_map<uint32_t, Layout> objects_;
};

// Rounds |value| up to a multiple of |align|.
uint32_t AlignUp(uint32_t value, uint32_t align);

}  // namespace abi
}  // namespace wolang

#endif  // WOLANG_ABI_LAYOUT_H_
