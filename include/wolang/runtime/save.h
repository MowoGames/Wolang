// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_RUNTIME_SAVE_H_
#define WOLANG_RUNTIME_SAVE_H_

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace wolang {
namespace runtime {

// Why a save could not be written or read back (D266).
enum class SaveStatus : int32_t {
  kOk = 0,
  // The bytes are not a save, or end early, or contradict themselves.
  kCorrupt = 1,
  // The bytes name a class the schema does not know.
  kUnknownClass = 2,
  // The schema text is malformed, or names a type it does not declare.
  kBadSchema = 3,
};

// An object to write, and the class the schema knows it as.
struct SaveRoot {
  void* Object = nullptr;
  std::string_view Class;
};

// Writes the object graph reachable from |roots| through the fields the
// |schema| describes. Each object is written once, however many fields refer
// to it, and a field is written under its name and a code for its type, so a
// reader built from another version of the program can tell which fields it
// still has (D266). |offsets| is the table the schema's `@k` places index; it
// may be null when the schema spells every number out.
SaveStatus SaveEncode(std::string_view schema, const uint64_t* offsets, std::span<const SaveRoot> roots, std::vector<uint8_t>* out);

// Rebuilds the graph |data| holds, as objects of the program |schema|
// describes. A field the bytes have and the schema lacks, or holds under
// another type code, is dropped; a field the schema has and the bytes lack
// stays zero. Nothing is allocated unless the whole of |data| reads cleanly.
// Every root comes back with one reference for the caller, and a root the
// bytes hold as null comes back null.
SaveStatus SaveDecode(std::string_view schema, const uint64_t* offsets, std::span<const uint8_t> data, std::vector<void*>* roots);

extern "C" {

// The bytes of a save of the object |root|, in storage `Free` gives back, and
// their count in |out_length|. Null with a length of zero when the schema or
// the graph cannot be written. The schema's ROOT line names the class.
// |offsets| is given back to the allocator before this returns.
uint8_t* wolang_save_encode(const uint8_t* schema, uint64_t schema_length, const uint64_t* offsets, uint64_t root, uint64_t* out_length);

// The object a save holds, or zero with |status| set to a SaveStatus.
uint64_t wolang_save_decode(const uint8_t* schema, uint64_t schema_length, const uint64_t* offsets, const uint8_t* data, uint64_t data_length, int32_t* status);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_SAVE_H_
