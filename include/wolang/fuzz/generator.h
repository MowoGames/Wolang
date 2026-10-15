// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_FUZZ_GENERATOR_H_
#define WOLANG_FUZZ_GENERATOR_H_

#include <cstdint>
#include <string>

namespace wolang {
namespace fuzz {

// Programs to run through every tier and compare.
//
// The three tiers are meant to agree: the interpreter on the IR as
// lowering left it is the reference, the interpreter on the optimized IR
// says whether the passes kept the meaning, and the machine code says
// whether the back end agrees with both. Every test written by hand checks
// that for one program; this writes the programs.
//
// What it generates is deliberately narrow and deliberately deterministic:
// integer and float arithmetic, comparisons, `if`, `while`, arrays,
// structs, and calls between generated functions. Narrow because a
// disagreement is only useful if it can be read; deterministic because a
// seed that fails has to fail again.
//
// Traps are part of the comparison rather than something to avoid. A
// division by zero and an overflow are defined outcomes, so the tiers have
// to produce the same one -- a tier that wraps where another traps is the
// bug this is looking for.
struct Options {
  uint64_t seed = 0;
  // How many functions besides the entry point.
  int functions = 3;
  // How deep an expression may nest.
  int depth = 3;
  // How many statements a body may have.
  int statements = 5;
  // Whether to generate floating-point arithmetic as well as integer.
  bool floats = true;
  // Whether to generate what can trap: division, remainder, and an index
  // that may be out of bounds.
  bool traps = true;
};

// A whole program, with an entry point named `Probe` returning `int`.
std::string Generate(const Options& options);

}  // namespace fuzz
}  // namespace wolang

#endif  // WOLANG_FUZZ_GENERATOR_H_
