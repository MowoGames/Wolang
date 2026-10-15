// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_ABI_LAYOUT_H_
#define WOLANG_ABI_LAYOUT_H_

#include <cstdint>
#include <span>
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

  // Another file's member tables, for a type this one does not name.
  //
  // A layout is a question about a *type*, and a type crosses files: a
  // `List<int>` built in one file of a package is laid out by whichever
  // file is being run, which may never have mentioned `List`. Without
  // this, such a type came back with no fields at all -- and a field
  // address then fell back to offset zero, which wrote one field over
  // another (D125).
  void Also(const FileResolution* resolution) {
    if (resolution != nullptr && resolution != &resolution_) {
      also_.push_back(resolution);
    }
  }

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

  // Where field |field| of element |index| sits in storage holding |count|
  // records as one array per field.
  //
  // The whole of what `soa` means: the same record, the same fields, and a
  // walk over one of them touching every byte of every cache line it
  // reads instead of one field's worth.
  //
  // Each field's array holds `SoaRoundCount(count)` slots rather than
  // `count`, which is what keeps the arithmetic cheap: the start of a
  // field's array is then a constant -- the sizes of the fields before it
  // -- times that one number, so an address costs one multiply instead of
  // one per field. The padding also leaves every array 16-byte aligned,
  // which is what a vector load wants.
  uint64_t SoaOffsetOf(TypeId record,
                       uint32_t field,
                       uint64_t index,
                       uint64_t count);
  // How many bytes |count| records take in that layout.
  uint64_t SoaSizeOf(TypeId record, uint64_t count);
  // The sizes of the fields before |field|, which is the multiplier of the
  // padded count in an offset.
  uint64_t SoaPrefixOf(TypeId record, uint32_t field);
  // How many slots each field's array really holds.
  static uint64_t SoaRoundCount(uint64_t count) {
    return (count + 15) / 16 * 16;
  }
  uint32_t SizeOf(TypeId type) { return Of(type).size; }
  uint32_t AlignOf(TypeId type) { return Of(type).align; }
  // The type of field |index| of an aggregate, or an invalid id -- which
  // is what an enum's tag is: its type is the instruction's, not the
  // declaration's, because no member declares it.
  TypeId FieldType(TypeId type, uint32_t index) const;
  // How many fields an aggregate has.
  uint32_t FieldCount(TypeId type) const;
  // Whether an object of |type| begins with a class descriptor: the number
  // that says which class it really is, which is what a virtual call reads
  // (D85). True for a class with a virtual method anywhere in its chain.
  bool HasHeader(TypeId type) const;
  // How many fields the classes |type| derives from contribute, which is
  // where its own fields start. The base's fields come first so that a
  // handle to a derived object is a handle to a base object at the same
  // address, and an upcast is nothing at run time.
  uint32_t InheritedFieldCount(TypeId type) const;
  // The distance between two elements of an array, which is the element's
  // size rounded up to its alignment.
  uint32_t StrideOf(TypeId array);
  // True for an enum whose variants carry nothing: the value is the tag
  // lowering reads as field zero and nothing else.
  bool IsTagOnly(TypeId type) const;

  // True for an enum where some variant carries values: the tag, and a
  // payload area big enough for the largest of them (D89).
  bool IsTagged(TypeId type) const;
  // Where the payload area starts, and how big it is.
  uint32_t PayloadOffsetOf(TypeId type);
  uint32_t PayloadSizeOf(TypeId type);
  // How much room one variant's values need. |variant| is the member's
  // function type: what it takes is what it carries.
  void MeasurePayload(TypeId variant, uint32_t* size, uint32_t* align);
  // The type of the |position|-th value variant |index| carries, or an
  // invalid id when it carries no such value.
  TypeId VariantFieldType(TypeId type,
                          uint32_t index,
                          uint32_t position) const;
  // Where that value sits in an enum of |type|. Every variant's values
  // start at the payload offset, which is what lets one site read them
  // once the tag has been checked.
  uint32_t VariantFieldOffset(TypeId type,
                              uint32_t index,
                              uint32_t position);

  // The width a scalar occupies in a register, which is what tells `ldr`
  // from `ldrb` and `fadd s` from `fadd d`.
  uint32_t BitsOf(TypeId type) const;
  bool IsSignedInteger(TypeId type) const;

 private:
  Layout Compute(TypeId type);
  Layout ComputeFields(TypeId type);

  const TypeStore& types_;
  // The members of |type|, from whichever file knows them.
  std::span<const Member> Members(TypeId type) const;

  const FileResolution& resolution_;
  std::vector<const FileResolution*> also_;
  std::unordered_map<uint32_t, Layout> cache_;
  // The storage a class points at, kept apart from the handle's layout.
  std::unordered_map<uint32_t, Layout> objects_;
};

// Rounds |value| up to a multiple of |align|.
uint32_t AlignUp(uint32_t value, uint32_t align);

}  // namespace abi
}  // namespace wolang

#endif  // WOLANG_ABI_LAYOUT_H_
