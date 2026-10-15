// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_INTERP_MARSHAL_H_
#define WOLANG_INTERP_MARSHAL_H_

#include <cstdint>

#include "wolang/abi/layout.h"
#include "wolang/interp/value.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace interp {

// A value as bytes, and bytes as a value.
//
// Two places need this and they have to agree: the host calling into
// compiled code, and the interpreter handing a buffer to a C function.
// When each had its own copy they could disagree about where a field sits,
// and a disagreement about layout is the kind of bug that looks like
// arithmetic going wrong.
//
// The layout is the one in `abi`; this is only the walk over it.

uint64_t CanonicalBits(abi::LayoutStore* layouts,
                       const Value& value,
                       TypeId type);

void Encode(const TypeStore& types,
            abi::LayoutStore* layouts,
            const Value& value,
            TypeId type,
            uint8_t* destination);

Value Decode(const TypeStore& types,
             abi::LayoutStore* layouts,
             const uint8_t* source,
             TypeId type);

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_MARSHAL_H_
