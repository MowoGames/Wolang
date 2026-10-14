// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_CLASSES_H_
#define WOLANG_RUNTIME_CLASSES_H_

#include <cstdint>

namespace wolang {
namespace runtime {

// Where a virtual call finds its body, for the tier that runs real code.
//
// An object carries a token saying which class it is -- its declaration,
// packed (see `ir::ClassTokenOf`) -- and a call names the method by its
// declaration too. The reference tier can turn that pair into a function
// by looking through the modules it holds; compiled code cannot, so the
// compiler registers the addresses it produced here and the call asks for
// one.
//
// Registered rather than baked into the code because a class may be
// created in one module and dispatched in another, and because the two
// tiers have to agree about what the number in the object means. A lookup
// is a C call per virtual call, which is the price of that agreement; the
// obvious next step is for the compiler to inline the table walk, which
// needs the table's address to be stable -- it is, which is why this is
// one array and not a hash map.
extern "C" {

// Says that the class |token| implements the method |selector| at |code|.
void wolang_class_register(uint64_t token, uint64_t selector, void* code);

// The code address implementing a method, or null when nothing registered
// one -- which is a program whose class was only ever interpreted.
void* wolang_class_lookup(uint64_t token, uint64_t selector);

// Forgets everything. Called when a compiled module is thrown away, so
// that a second compilation in the same process does not see the first
// one's addresses.
void wolang_class_reset(void);

}  // extern "C"

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_CLASSES_H_
