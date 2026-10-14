// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_CLASSES_H_
#define WOLANG_RUNTIME_CLASSES_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Where a virtual call finds its body, for the tier that runs real code.
//
// An object carries a token saying which class it is -- its declaration,
// packed (see `ir::ClassTokenOf`). The reference tier can turn that into a
// function by looking through the modules it holds; compiled code cannot,
// so the compiler registers the addresses it produced here and the call
// asks for one.
//
// Registered rather than baked into the code because a class may be
// created in one module and dispatched in another, and because the two
// tiers have to agree about what the number in the object means. A lookup
// is a C call per virtual call, which is the price of that agreement; the
// obvious next step is for the compiler to inline the table walk, which
// needs the table's address to be stable -- it is, which is why this is
// one array and not a hash map.
extern "C" {

// Says that slot |slot| of the class |token| is at |code|.
void wolang_class_register(uint64_t token, uint32_t slot, void* code);

// The code address for a slot, or null when nothing registered one --
// which is a program whose class was only ever interpreted.
void* wolang_class_lookup(uint64_t token, uint32_t slot);

// Forgets everything. Called when a compiled module is thrown away, so
// that a second compilation in the same process does not see the first
// one's addresses.
void wolang_class_reset(void);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_CLASSES_H_
