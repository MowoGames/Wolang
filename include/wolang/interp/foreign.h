// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_INTERP_FOREIGN_H_
#define WOLANG_INTERP_FOREIGN_H_

#include <cstdint>
#include <span>
#include <string_view>

namespace wolang {
namespace interp {

// One argument of a foreign call, reduced to its bits and the register file
// it belongs in. Which is all the C ABI needs to know about it.
struct ForeignArgument {
  uint64_t bits = 0;
  bool is_float = false;
};

// Calling out of the language is the one thing the interpreter cannot do by
// itself: putting arguments where the C ABI wants them is machine code.
//
// So it is handed one of these rather than depending on the code generator
// that can make it. Both tiers then reach C the same way, which is what
// keeps a program that calls out comparable between them -- and a trap in
// either is still a trap in both.
class ForeignCaller {
 public:
  virtual ~ForeignCaller() = default;

  // Calls |symbol| and writes the raw result. Returns false when the symbol
  // is not loaded, which the caller turns into a trap.
  virtual bool Call(std::string_view symbol,
                    std::span<const ForeignArgument> arguments,
                    bool returns_float,
                    uint64_t* result) = 0;
};

}  // namespace interp
}  // namespace wolang

#endif  // WOLANG_INTERP_FOREIGN_H_
