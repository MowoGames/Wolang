// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_PRINTER_H_
#define WOLANG_IR_PRINTER_H_

#include <string>

#include "wolang/base/string_interner.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace ir {

// Renders the IR in a stable textual form. "Stable" is the requirement, not
// a nicety: golden tests compare this text, and wave 12 compares the
// interpreter, the JIT and the AOT tier against each other by replaying what
// this prints.
//
// Every value is named `%n` by its instruction index, and every block `^n`,
// so the text says what the data structure says and nothing more.
std::string PrintFunction(const StringInterner& interner,
                          const TypeStore& types,
                          const Module& module,
                          const Function& function);

std::string PrintModule(const StringInterner& interner,
                        const TypeStore& types,
                        const Module& module);

// The typed form: everything the reader in `ir/reader.h` needs to rebuild the
// module, which the stable form above leaves out on purpose (D264). It is a
// separate entry point so the golden tests that compare the stable form do
// not change.
std::string PrintModuleTyped(const StringInterner& interner,
                             const TypeStore& types,
                             const Module& module);

// One type in the typed form's grammar, which unlike `Describe` has no two
// types with one spelling. A kind the reader cannot build is written in
// backticks as `Describe` spells it, and the reader looks it up by that text.
std::string PrintType(const StringInterner& interner,
                      const TypeStore& types,
                      TypeId type);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_PRINTER_H_
