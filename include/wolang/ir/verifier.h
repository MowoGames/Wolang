// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_VERIFIER_H_
#define WOLANG_IR_VERIFIER_H_

#include <string>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace ir {

// Everything the verifier found wrong, as text meant for a developer of the
// compiler rather than for a user of the language. An empty result is the
// only acceptable one: the IR is the contract between the front end and
// every back end, and a violation is a compiler bug.
struct VerificationResult {
  std::vector<std::string> problems;

  bool ok() const { return problems.empty(); }
};

// Checks the structural invariants of |function|:
//
//   * every block ends with exactly one terminator, and has one nowhere else;
//   * every operand is defined before it is used, on every path that reaches
//     the use -- which is dominance;
//   * a jump passes exactly as many arguments as its target has parameters,
//     with matching types;
//   * every block is reachable from the entry;
//   * operand types fit the operation.
VerificationResult Verify(const StringInterner& interner,
                          const TypeStore& types,
                          const Module& module,
                          const Function& function);

VerificationResult Verify(const StringInterner& interner,
                          const TypeStore& types,
                          const Module& module);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_VERIFIER_H_
