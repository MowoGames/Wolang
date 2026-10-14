// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/builder.h"

namespace wolang {
namespace ir {

BlockId Builder::AddBlock(std::string label) {
  const BlockId id{static_cast<uint32_t>(function().blocks.size())};
  Block block;
  block.label = std::move(label);
  function().blocks.push_back(std::move(block));
  return id;
}

bool Builder::Terminated() const {
  if (!function().Contains(current_)) {
    return true;
  }
  const InstId last = function().TerminatorOf(current_);
  return function().Contains(last) &&
         IsTerminator(function().Get(last).opcode);
}

InstId Builder::Append(Inst inst, BlockId block) {
  const InstId id{static_cast<uint32_t>(function().instructions.size())};
  function().instructions.push_back(std::move(inst));
  function().owners.push_back(block);
  function().Get(block).instructions.push_back(id);
  return id;
}

InstId Builder::AddParam(BlockId block, TypeId type) {
  Inst inst;
  inst.opcode = Opcode::kParam;
  inst.type = type;

  const InstId id{static_cast<uint32_t>(function().instructions.size())};
  function().instructions.push_back(std::move(inst));
  function().owners.push_back(block);

  Block& target = function().Get(block);
  target.instructions.insert(
      target.instructions.begin() + target.param_count, id);
  ++target.param_count;
  return id;
}

InstId Builder::Add(Opcode opcode,
                    TypeId type,
                    std::vector<InstId> operands) {
  if (Terminated()) {
    return InstId{};
  }
  Inst inst;
  inst.opcode = opcode;
  inst.type = type;
  inst.operands_begin = static_cast<uint32_t>(function().operands.size());
  inst.operands_count = static_cast<uint32_t>(operands.size());
  function().operands.insert(function().operands.end(), operands.begin(),
                             operands.end());
  return Append(std::move(inst), current_);
}

InstId Builder::ConstInt(TypeId type, uint64_t value) {
  const InstId id = Add(Opcode::kConstInt, type);
  if (function().Contains(id)) {
    function().Get(id).payload.integer = value;
  }
  return id;
}

InstId Builder::ConstFloat(TypeId type, double value) {
  const InstId id = Add(Opcode::kConstFloat, type);
  if (function().Contains(id)) {
    function().Get(id).payload.floating = value;
  }
  return id;
}

InstId Builder::ConstBool(bool value) {
  const InstId id = Add(Opcode::kConstBool, types->bool_type());
  if (function().Contains(id)) {
    function().Get(id).payload.boolean = value;
  }
  return id;
}

InstId Builder::ConstNull(TypeId type) {
  return Add(Opcode::kConstNull, type);
}

InstId Builder::ConstString(TypeId type, std::string text) {
  const InstId id = Add(Opcode::kConstString, type);
  if (function().Contains(id)) {
    function().Get(id).payload.string =
        static_cast<uint32_t>(module_->strings.size());
    module_->strings.push_back(std::move(text));
  }
  return id;
}

InstId Builder::Undef(TypeId type) {
  return Add(Opcode::kUndef, type);
}

InstId Builder::Call(uint32_t callee,
                     TypeId result,
                     std::vector<InstId> arguments) {
  const InstId id = Add(Opcode::kCall, result, std::move(arguments));
  if (function().Contains(id)) {
    function().Get(id).payload.callee = callee;
  }
  return id;
}

InstId Builder::Extract(TypeId type, InstId aggregate, uint32_t field) {
  const InstId id = Add(Opcode::kExtract, type, {aggregate});
  if (function().Contains(id)) {
    function().Get(id).field = field;
  }
  return id;
}

InstId Builder::FieldAddress(TypeId pointer, InstId base, uint32_t field) {
  const InstId id = Add(Opcode::kFieldAddress, pointer, {base});
  if (function().Contains(id)) {
    function().Get(id).field = field;
  }
  return id;
}

InstId Builder::Insert(TypeId type,
                       InstId aggregate,
                       InstId value,
                       uint32_t field) {
  const InstId id = Add(Opcode::kInsert, type, {aggregate, value});
  if (function().Contains(id)) {
    function().Get(id).field = field;
  }
  return id;
}

void Builder::Return(InstId value) {
  Add(Opcode::kReturn, TypeId{}, {value});
}

void Builder::ReturnVoid() {
  Add(Opcode::kReturn, TypeId{});
}

void Builder::Jump(BlockId target, std::vector<InstId> arguments) {
  const InstId id = Add(Opcode::kJump, TypeId{}, std::move(arguments));
  if (!function().Contains(id)) {
    return;
  }
  Inst& inst = function().Get(id);
  inst.successors_begin = static_cast<uint32_t>(function().successors.size());
  inst.successors_count = 1;
  function().successors.push_back(target);
}

void Builder::Branch(InstId condition, BlockId yes, BlockId no) {
  const InstId id = Add(Opcode::kBranch, TypeId{}, {condition});
  if (!function().Contains(id)) {
    return;
  }
  Inst& inst = function().Get(id);
  inst.successors_begin = static_cast<uint32_t>(function().successors.size());
  inst.successors_count = 2;
  function().successors.push_back(yes);
  function().successors.push_back(no);
}

void Builder::Switch(InstId subject,
                     std::vector<uint64_t> values,
                     std::vector<BlockId> targets,
                     BlockId fallback) {
  const InstId id = Add(Opcode::kSwitch, TypeId{}, {subject});
  if (!function().Contains(id)) {
    return;
  }
  Inst& inst = function().Get(id);
  inst.cases_begin = static_cast<uint32_t>(function().cases.size());
  inst.cases_count = static_cast<uint32_t>(values.size());
  function().cases.insert(function().cases.end(), values.begin(),
                          values.end());

  inst.successors_begin = static_cast<uint32_t>(function().successors.size());
  // The default comes last, which is what lets the case values and the first
  // |cases_count| successors stay parallel.
  inst.successors_count = static_cast<uint32_t>(targets.size()) + 1;
  function().successors.insert(function().successors.end(), targets.begin(),
                               targets.end());
  function().successors.push_back(fallback);
}

void Builder::Unreachable() {
  Add(Opcode::kUnreachable, TypeId{});
}

}  // namespace ir
}  // namespace wolang
