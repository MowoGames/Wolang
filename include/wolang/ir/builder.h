// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_BUILDER_H_
#define WOLANG_IR_BUILDER_H_

#include <cstddef>
#include <cstdint>
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

  // The instruction, for the caller that has to set a payload the
  // builder's own methods do not take. Asked through the builder rather
  // than through the module, because only the builder knows which
  // function is being built.
  Inst& Mutable(InstId id) {
    return module_->functions[function_.index].Get(id);
  }

  BlockId AddBlock(std::string label);

  BlockId current() const { return current_; }
  void SetCurrent(BlockId block) {
    current_ = block;
    poisoned_ = false;
  }

  // Says that nothing reaches the current block, so nothing should be
  // put in it.
  //
  // Where this comes up: an `if` whose every arm returns or breaks has a
  // join block nothing jumps to, and whatever the enclosing structure
  // would have emitted there is unreachable too. An *empty* unreachable
  // block is something the verifier and the passes both ignore; one with
  // a jump in it is a block the verifier is right to complain about
  // (D152).
  void Poison() { poisoned_ = true; }

  // Where the instructions being added came from. Lowering moves this as
  // it walks, and everything appended until it moves again belongs to
  // that span (D118).
  void SetWhere(SourceSpan span) { where_ = span; }
  SourceSpan where() const { return where_; }

  // True when the current block already has a terminator, so nothing more
  // can be added to it.
  bool Terminated() const;

  // A parameter of |block|. Parameters have to be added before anything
  // else in their block.
  InstId AddParam(BlockId block, TypeId type);

  // An instruction at the top of the entry block, before whatever it
  // ends with.
  //
  // For a definition that has to dominate everything: a closure's
  // environment is allocated once per call and freed when the call ends,
  // and allocating it where the closure is written would leave it
  // undominated by the scope exit that frees it (D138).
  // Several of them stay in the order they were asked for, so one may use
  // another.
  InstId AddToEntry(Opcode opcode, TypeId type, std::vector<InstId> operands = {});


  InstId Add(Opcode opcode, TypeId type, std::vector<InstId> operands = {});

  InstId ConstInt(TypeId type, uint64_t value);
  InstId ConstFloat(TypeId type, double value);
  InstId ConstBool(bool value);
  InstId ConstNull(TypeId type);
  InstId ConstString(TypeId type, std::string text);
  InstId Undef(TypeId type);

  InstId Call(uint32_t callee, TypeId result, std::vector<InstId> arguments);

  InstId Extract(TypeId type, InstId aggregate, uint32_t field);
  // The address of a field, given the address of the aggregate. What a
  // method's `self` is read and written through: the receiver arrives as a
  // pointer, so a field of it is an address rather than a copy.
  InstId FieldAddress(TypeId pointer, InstId base, uint32_t field);
  // The address of an element, given the address of the array and the
  // index as a value. What writing one element of a buffer goes through.
  InstId IndexAddress(TypeId pointer,
                      InstId base,
                      InstId index,
                      InstId length);
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
  bool poisoned_ = false;
  size_t entry_added_ = 0;
  SourceSpan where_;
};

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_BUILDER_H_
