// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_SAVE_SCHEMA_H_
#define WOLANG_IR_SAVE_SCHEMA_H_

#include <cstdint>
#include <functional>
#include <string>

#include "wolang/base/string_interner.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace ir {

// What the schema writer needs from whoever asks for one: lowering has no
// layout and leaves `@k` places for the tier to fill in, the embedding API
// has the layout and writes the numbers (D266, D292).
struct SaveSchemaHost {
  TypeStore* types = nullptr;
  StringInterner* interner = nullptr;
  std::function<const FileResolution*(TypeId)> resolution;
  std::function<TypeId(TypeId)> erase;
  // Where field |field| of |type| sits, or how big its object is when
  // |field| is `kObjectSizeField`, as the text the schema carries.
  std::function<std::string(TypeId type, uint32_t field)> offset;
};

// The text `wolang_save_encode` and `wolang_save_decode` read for the
// graph reachable from the class |root| through fields marked `@Save`.
// Empty when |root| cannot be saved or a marked field holds something that
// cannot be written (D266).
std::string BuildSaveSchema(const SaveSchemaHost& host, TypeId root);

// Whether |type|, or a class it derives from, has a virtual method: such an
// object has a descriptor and cannot be rebuilt from a save (D266).
bool SaveHasVirtualMethods(const SaveSchemaHost& host, TypeId type);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_SAVE_SCHEMA_H_
