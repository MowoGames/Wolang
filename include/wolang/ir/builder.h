// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_IR_BUILDER_H_
#define WOLANG_IR_BUILDER_H_

#include <string>
#include <utility>
#include <vector>

#include "wolang/ir/ir.h"

namespace wolang {
namespace ir {

// Assembles one function. Keeps the three parallel pools (operands,
// successors, cases) consistent with the instructions that index into them,
// which is the part a hand written lowering gets wrong.
//
// The builder has a current block. Appending after the block is terminated is
// a no-op rather than an error: lowering a `return` in the middle of a body
// leaves the rest of the statements with nowhere to go, and dropping them
// here is what keeps the verifier's "exactly one terminator" invariant true
// without the lowering having to track reachability itself.
class Builder {
 public:
  // Holds an index rather than a pointer: adding a declaration for an
  // imported callee grows the module's function vector, and a pointer into
  // it would dangle the moment that happens.
  Builder(Module* module, FunctionId function)
      : module_(module), function_(function) {}

  BlockId AddBlock(std::string label);

  BlockId current() const { return current_; }
  void SetCurrent(BlockId block) { current_ = block; }

  // True when the current block already has a terminator, so nothing more
  // can be added to it.
  bool Terminated() const;

  // A parameter of |block|. Parameters have to be added before anything
  // else in their block.
  InstId AddParam(BlockId block, TypeId type);

  InstId Add(Opcode opcode, TypeId type, std::vector<InstId> operands = {});

  InstId ConstInt(TypeId type, uint64_t value);
  InstId ConstFloat(TypeId type, double value);
  InstId ConstBool(bool value);
  InstId ConstNull(TypeId type);
  InstId ConstString(TypeId type, std::string text);
  InstId Undef(TypeId type);

  InstId Call(uint32_t callee, TypeId result, std::vector<InstId> arguments);

  InstId Extract(TypeId type, InstId aggregate, uint32_t field);
  InstId Insert(TypeId type, InstId aggregate, InstId value, uint32_t field);

  // Terminators. Each ends the current block; a second one is dropped.
  void Return(InstId value);
  void ReturnVoid();
  void Jump(BlockId target, std::vector<InstId> arguments = {});
  void Branch(InstId condition, BlockId yes, BlockId no);
  void Switch(InstId subject,
              std::vector<uint64_t> values,
              std::vector<BlockId> targets,
              BlockId fallback);
  void Unreachable();

  TypeStore* types = nullptr;

 private:
  InstId Append(Inst inst, BlockId block);

  Function& function() { return module_->functions[function_.index]; }
  const Function& function() const {
    return module_->functions[function_.index];
  }

  Module* module_;
  FunctionId function_;
  BlockId current_;
};

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_BUILDER_H_
