// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/passes.h"

#include <algorithm>

namespace wolang {
namespace ir {
namespace {

// Turns |id| into something nothing depends on, without disturbing the
// numbering every operand refers to.
void Discard(Function* function, InstId id) {
  Inst& inst = function->Get(id);
  inst.opcode = Opcode::kUndef;
  inst.operands_count = 0;
  inst.successors_count = 0;
  inst.cases_count = 0;
}

// Replaces every use of |from| with |to|.
bool ReplaceAllUses(Function* function, InstId from, InstId to) {
  bool changed = false;
  for (InstId& operand : function->operands) {
    if (operand == from) {
      operand = to;
      changed = true;
    }
  }
  return changed;
}

// An operation with no operands and no effect on anything outside itself.
bool IsPure(Opcode opcode) {
  switch (opcode) {
    case Opcode::kStore:
    case Opcode::kCall:
    case Opcode::kCallIndirect:
    case Opcode::kAlloca:
      return false;
    default:
      return !IsTerminator(opcode);
  }
}

}  // namespace

// ------------------------------------------------------ constant folding ---

bool FoldConstants(TypeStore* types, Function* function) {
  bool changed = false;
  for (uint32_t i = 0; i < function->instructions.size(); ++i) {
    const InstId id{i};
    Inst& inst = function->Get(id);
    const std::span<const InstId> operands = function->OperandsOf(inst);

    // Integer arithmetic and comparison on two constants.
    if (operands.size() == 2) {
      const Inst& left = function->Get(operands[0]);
      const Inst& right = function->Get(operands[1]);
      if (left.opcode != Opcode::kConstInt ||
          right.opcode != Opcode::kConstInt) {
        continue;
      }
      const uint64_t a = left.payload.integer;
      const uint64_t b = right.payload.integer;

      bool folded = true;
      uint64_t value = 0;
      bool boolean = false;
      bool produces_bool = false;
      switch (inst.opcode) {
        case Opcode::kAdd:
          value = a + b;
          break;
        case Opcode::kSub:
          value = a - b;
          break;
        case Opcode::kMul:
          value = a * b;
          break;
        case Opcode::kDiv:
          // Division by zero is a runtime fault, not something to fold into
          // a wrong answer at compile time.
          folded = b != 0;
          value = folded ? a / b : 0;
          break;
        case Opcode::kRem:
          folded = b != 0;
          value = folded ? a % b : 0;
          break;
        case Opcode::kBitAnd:
          value = a & b;
          break;
        case Opcode::kBitOr:
          value = a | b;
          break;
        case Opcode::kBitXor:
          value = a ^ b;
          break;
        case Opcode::kEq:
          boolean = a == b;
          produces_bool = true;
          break;
        case Opcode::kNe:
          boolean = a != b;
          produces_bool = true;
          break;
        case Opcode::kLt:
          boolean = a < b;
          produces_bool = true;
          break;
        case Opcode::kLe:
          boolean = a <= b;
          produces_bool = true;
          break;
        case Opcode::kGt:
          boolean = a > b;
          produces_bool = true;
          break;
        case Opcode::kGe:
          boolean = a >= b;
          produces_bool = true;
          break;
        default:
          folded = false;
          break;
      }
      if (!folded) {
        continue;
      }

      inst.operands_count = 0;
      if (produces_bool) {
        inst.opcode = Opcode::kConstBool;
        inst.type = types->bool_type();
        inst.payload.boolean = boolean;
      } else {
        inst.opcode = Opcode::kConstInt;
        inst.payload.integer = value;
      }
      changed = true;
      continue;
    }

    if (operands.size() != 1) {
      continue;
    }
    const Inst& only = function->Get(operands[0]);
    if (inst.opcode == Opcode::kLogicalNot &&
        only.opcode == Opcode::kConstBool) {
      inst.opcode = Opcode::kConstBool;
      inst.operands_count = 0;
      inst.payload.boolean = !only.payload.boolean;
      changed = true;
    } else if (inst.opcode == Opcode::kNeg &&
               only.opcode == Opcode::kConstInt) {
      inst.opcode = Opcode::kConstInt;
      inst.operands_count = 0;
      inst.payload.integer = ~only.payload.integer + 1;
      changed = true;
    }
  }
  return changed;
}

// ----------------------------------------------------- dead code removal ---

bool EliminateDeadCode(TypeStore* types,
                       const Module& module,
                       Function* function) {
  static_cast<void>(types);
  std::vector<bool> used(function->instructions.size(), false);

  // A terminator's operands and anything an impure instruction reads are
  // live; so is everything they reach.
  std::vector<InstId> worklist;
  for (uint32_t i = 0; i < function->instructions.size(); ++i) {
    const Inst& inst = function->Get(InstId{i});
    const bool keep =
        IsTerminator(inst.opcode) || !IsPure(inst.opcode) ||
        // A call with effects has to stay even if its result is unused; one
        // without is exactly what the effect row licenses removing.
        (inst.opcode == Opcode::kCall &&
         inst.payload.callee < module.functions.size() &&
         !module.functions[inst.payload.callee].effects.empty());
    if (!keep) {
      continue;
    }
    for (InstId operand : function->OperandsOf(inst)) {
      worklist.push_back(operand);
    }
  }

  while (!worklist.empty()) {
    const InstId id = worklist.back();
    worklist.pop_back();
    if (!function->Contains(id) || used[id.index]) {
      continue;
    }
    used[id.index] = true;
    for (InstId operand : function->OperandsOf(function->Get(id))) {
      worklist.push_back(operand);
    }
  }

  // A slot nothing reads is dead, and so are the stores into it. Promotion
  // leaves exactly that behind, so without this the `alloca` would survive
  // every `var` the pass had just made unnecessary.
  bool changed = false;
  for (uint32_t i = 0; i < function->instructions.size(); ++i) {
    const InstId slot{i};
    if (function->Get(slot).opcode != Opcode::kAlloca) {
      continue;
    }
    std::vector<InstId> writers;
    bool read = false;
    for (uint32_t j = 0; j < function->instructions.size(); ++j) {
      const Inst& user = function->Get(InstId{j});
      const std::span<const InstId> operands = function->OperandsOf(user);
      if (std::find(operands.begin(), operands.end(), slot) ==
          operands.end()) {
        continue;
      }
      if (user.opcode == Opcode::kStore && !operands.empty() &&
          operands[0] == slot) {
        writers.push_back(InstId{j});
        continue;
      }
      read = true;
      break;
    }
    if (read) {
      continue;
    }
    writers.push_back(slot);
    for (InstId id : writers) {
      Block& block = function->Get(function->owners[id.index]);
      const auto position = std::find(block.instructions.begin(),
                                      block.instructions.end(), id);
      if (position == block.instructions.end()) {
        continue;
      }
      Discard(function, id);
      block.instructions.erase(position);
      changed = true;
    }
  }

  for (uint32_t i = 0; i < function->instructions.size(); ++i) {
    const InstId id{i};
    Inst& inst = function->Get(id);
    if (used[i] || IsTerminator(inst.opcode) || !IsPure(inst.opcode) ||
        inst.opcode == Opcode::kUndef || inst.opcode == Opcode::kParam) {
      continue;
    }
    // Nothing reads it and it cannot do anything, so it is gone.
    Discard(function, id);
    Block& block = function->Get(function->owners[i]);
    const auto position = std::find(block.instructions.begin(),
                                    block.instructions.end(), id);
    if (position != block.instructions.end()) {
      block.instructions.erase(position);
      changed = true;
    }
  }
  return changed;
}

// --------------------------------------------------------- slot promotion ---

bool PromoteSlots(TypeStore* types, Function* function) {
  static_cast<void>(types);
  bool changed = false;

  for (uint32_t i = 0; i < function->instructions.size(); ++i) {
    const InstId slot{i};
    if (function->Get(slot).opcode != Opcode::kAlloca) {
      continue;
    }

    // Only a slot used exclusively by loads and stores can be promoted: if
    // its address goes anywhere else, something else may be writing it.
    bool only_loads_and_stores = true;
    std::vector<InstId> loads;
    std::vector<InstId> stores;
    for (uint32_t j = 0; j < function->instructions.size(); ++j) {
      const Inst& user = function->Get(InstId{j});
      const std::span<const InstId> operands = function->OperandsOf(user);
      const bool reads_slot =
          std::find(operands.begin(), operands.end(), slot) != operands.end();
      if (!reads_slot) {
        continue;
      }
      if (user.opcode == Opcode::kLoad) {
        loads.push_back(InstId{j});
      } else if (user.opcode == Opcode::kStore && !operands.empty() &&
                 operands[0] == slot) {
        stores.push_back(InstId{j});
      } else {
        only_loads_and_stores = false;
        break;
      }
    }
    if (!only_loads_and_stores) {
      continue;
    }

    // One store, and it is in the block that defines the slot: every load
    // then reads that one value. The general case needs block parameters
    // inserted on every join, which is the next step for this pass.
    if (stores.size() != 1) {
      continue;
    }
    const InstId store = stores.front();
    if (function->owners[store.index] != function->owners[slot.index]) {
      continue;
    }
    bool every_load_dominated = true;
    for (InstId load : loads) {
      if (function->owners[load.index] != function->owners[slot.index]) {
        every_load_dominated = false;
        break;
      }
    }
    if (!every_load_dominated) {
      continue;
    }

    const std::span<const InstId> operands =
        function->OperandsOf(function->Get(store));
    if (operands.size() != 2) {
      continue;
    }
    const InstId value = operands[1];

    for (InstId load : loads) {
      ReplaceAllUses(function, load, value);
      Discard(function, load);
      Block& block = function->Get(function->owners[load.index]);
      const auto position = std::find(block.instructions.begin(),
                                      block.instructions.end(), load);
      if (position != block.instructions.end()) {
        block.instructions.erase(position);
      }
    }
    changed = true;
  }
  return changed;
}

// ---------------------------------------------------------- control flow ---

bool SimplifyControlFlow(TypeStore* types, Function* function) {
  static_cast<void>(types);
  if (function->is_declaration()) {
    return false;
  }

  // A branch on a constant goes one way, so the other edge is gone.
  bool changed = false;
  for (uint32_t i = 0; i < function->blocks.size(); ++i) {
    const InstId terminator = function->TerminatorOf(BlockId{i});
    if (!function->Contains(terminator)) {
      continue;
    }
    Inst& inst = function->Get(terminator);
    if (inst.opcode != Opcode::kBranch || inst.operands_count != 1) {
      continue;
    }
    const Inst& condition =
        function->Get(function->operands[inst.operands_begin]);
    if (condition.opcode != Opcode::kConstBool) {
      continue;
    }
    const BlockId taken =
        function->successors[inst.successors_begin +
                             (condition.payload.boolean ? 0 : 1)];
    inst.opcode = Opcode::kJump;
    inst.operands_count = 0;
    inst.successors_begin = static_cast<uint32_t>(function->successors.size());
    inst.successors_count = 1;
    function->successors.push_back(taken);
    changed = true;
  }

  // Blocks nothing reaches any more are removed. Their instructions stay in
  // the pool, which is what keeps every other index meaningful.
  std::vector<bool> reachable(function->blocks.size(), false);
  std::vector<BlockId> worklist{function->entry()};
  reachable[0] = true;
  while (!worklist.empty()) {
    const BlockId block = worklist.back();
    worklist.pop_back();
    const InstId terminator = function->TerminatorOf(block);
    if (!function->Contains(terminator)) {
      continue;
    }
    for (BlockId next : function->SuccessorsOf(function->Get(terminator))) {
      if (!function->Contains(next) || reachable[next.index]) {
        continue;
      }
      reachable[next.index] = true;
      worklist.push_back(next);
    }
  }

  // A block is emptied rather than erased, because a BlockId is an index.
  // The verifier's reachability check is what would otherwise complain, so
  // it treats an empty unreachable block as gone.
  for (uint32_t i = 0; i < function->blocks.size(); ++i) {
    if (reachable[i] || function->blocks[i].instructions.empty()) {
      continue;
    }
    for (InstId id : function->blocks[i].instructions) {
      Discard(function, id);
    }
    function->blocks[i].instructions.clear();
    function->blocks[i].param_count = 0;
    changed = true;
  }
  return changed;
}

// ------------------------------------------------------------- the driver ---

void Optimize(TypeStore* types, const Module& module, Function* function) {
  if (function->is_declaration()) {
    return;
  }
  // To a fixpoint, bounded so that a pass that keeps reporting a change
  // cannot hang the compiler.
  for (int round = 0; round < 8; ++round) {
    bool changed = false;
    changed |= FoldConstants(types, function);
    changed |= SimplifyControlFlow(types, function);
    changed |= PromoteSlots(types, function);
    changed |= EliminateDeadCode(types, module, function);
    if (!changed) {
      return;
    }
  }
}

void Optimize(TypeStore* types, Module* module) {
  for (Function& function : module->functions) {
    Optimize(types, *module, &function);
  }
}

}  // namespace ir
}  // namespace wolang
