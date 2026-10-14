// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/passes.h"

#include "wolang/ir/analysis.h"

#include <algorithm>

#include "wolang/base/check.h"

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

// How many edges arrive at |block|. A block with one is a candidate for
// being merged into the block that jumps to it.
uint32_t EdgesInto(const Function& function, BlockId block) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < function.blocks.size(); ++i) {
    const InstId terminator = function.TerminatorOf(BlockId{i});
    if (!function.Contains(terminator)) {
      continue;
    }
    for (BlockId successor :
         function.SuccessorsOf(function.Get(terminator))) {
      count += successor == block ? 1 : 0;
    }
  }
  return count;
}

// True when removing the operation cannot change what the program does.
//
// Arithmetic is *not* pure in this language. Overflow and division by zero
// are traps, and a trap is a defined outcome the program can observe -- so
// deleting an unused `a + b` would turn a program that faults into one that
// does not. Removing it needs a proof that it cannot trap, which is range
// analysis and belongs with the optimizer proper.
//
// `extract` and `insert` are treated as pure even though the interpreter
// traps on a field that is not there: the front end builds them from the
// member table, so an out-of-range field is a compiler bug rather than
// program behaviour to preserve.
bool IsPure(Opcode opcode) {
  switch (opcode) {
    // Can trap.
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kMul:
    case Opcode::kDiv:
    case Opcode::kRem:
    case Opcode::kNeg:
    case Opcode::kShl:
    case Opcode::kShr:
    case Opcode::kIndex:
    case Opcode::kOptionalValue:
    case Opcode::kLoad:
    case Opcode::kUndef:
    // Does something, or is something.
    case Opcode::kIndexCheck:
    case Opcode::kStore:
    case Opcode::kCall:
    case Opcode::kWaitCall:
    case Opcode::kSpawnCall:
    case Opcode::kCallIndirect:
    case Opcode::kAlloca:
      return false;
    default:
      return !IsTerminator(opcode);
  }
}

// Whether two of these with the same operands must give the same answer.
//
// Wider than `IsPure`, and deliberately: arithmetic that can trap is not
// pure -- removing it could remove a trap -- but *replacing* one with an
// identical earlier one is safe, because if the earlier one trapped the
// later one was never reached. What is excluded is everything that reads
// or writes the world: a load, a store, a call, an allocation.
bool IsRepeatable(Opcode opcode) {
  switch (opcode) {
    case Opcode::kLoad:
    case Opcode::kStore:
    case Opcode::kCall:
    case Opcode::kWaitCall:
    case Opcode::kSpawnCall:
    case Opcode::kCallIndirect:
    case Opcode::kAlloca:
    case Opcode::kNew:
    case Opcode::kSoaNew:
    case Opcode::kUndef:
    case Opcode::kParam:
      return false;
    default:
      return !IsTerminator(opcode);
  }
}

// The record an `soa.addr` walks: the element of the storage its base
// points at, which is the one thing the instruction itself cannot say --
// its own type is the field's.
TypeId SoaRecordOf(const TypeStore& types,
                   const Function& function,
                   const Inst& inst) {
  const std::span<const InstId> operands = function.OperandsOf(inst);
  if (operands.empty() || !function.Contains(operands[0])) {
    return TypeId{};
  }
  const TypeId base = function.Get(operands[0]).type;
  return types.KindOf(base) == TypeKind::kPointer ? types.ElementOf(base)
                                                  : base;
}

// Whether two instructions compute the same thing.
bool SameComputation(const Function& function,
                     const Inst& left,
                     const Inst& right) {
  if (left.opcode != right.opcode || left.type != right.type ||
      left.field != right.field ||
      left.payload.integer != right.payload.integer) {
    return false;
  }
  const std::span<const InstId> a = function.OperandsOf(left);
  const std::span<const InstId> b = function.OperandsOf(right);
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
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

// ---------------------------------------------------------------- inlining ---

namespace {

constexpr uint32_t kNoBlock = 0xFFFFFFFFu;

// What a call has to look like to be worth replacing with the body it
// calls. The dev tier wants the small ones -- an accessor, a comparison, a
// wrapper around another call -- which are exactly the calls that cost more
// than they do. Anything larger is a decision for the optimizing tier,
// which will have a cost model to make it with.
bool WorthInlining(const Module& module,
                   const Function& caller,
                   const Inst& call) {
  constexpr size_t kMaxInstructions = 24;
  constexpr size_t kMaxBlocks = 4;

  if (call.opcode != Opcode::kCall ||
      call.payload.callee >= module.functions.size()) {
    return false;
  }
  const Function& callee = module.functions[call.payload.callee];
  if (callee.is_declaration() || &callee == &caller) {
    // A declaration has no body, and a function that calls itself would
    // inline forever. Mutual recursion is bounded by the driver's rounds.
    return false;
  }
  if (callee.instructions.size() > kMaxInstructions ||
      callee.blocks.size() > kMaxBlocks) {
    return false;
  }
  // A call the verifier would reject is not one to rewrite.
  return callee.ParamsOf(callee.entry()).size() == call.operands_count;
}

// Appends |inst| to |function| in |block|, returning its id.
InstId Append(Function* function, Inst inst, BlockId block) {
  const InstId id{static_cast<uint32_t>(function->instructions.size())};
  function->instructions.push_back(inst);
  function->owners.push_back(block);
  function->Get(block).instructions.push_back(id);
  return id;
}

}  // namespace

bool InlineCalls(TypeStore* types, const Module& module, Function* function) {
  if (function->is_declaration()) {
    return false;
  }

  // One call per invocation: the rewrite moves instructions between blocks,
  // and finding the next one on a fresh walk is cheaper than reasoning
  // about what the last one invalidated.
  InstId call_id;
  BlockId host_id;
  size_t position = 0;
  for (uint32_t b = 0; b < function->blocks.size() && !call_id.is_valid();
       ++b) {
    const Block& block = function->blocks[b];
    for (size_t i = 0; i < block.instructions.size(); ++i) {
      const InstId id = block.instructions[i];
      if (!WorthInlining(module, *function, function->Get(id))) {
        continue;
      }
      call_id = id;
      host_id = BlockId{b};
      position = i;
      break;
    }
  }
  if (!call_id.is_valid()) {
    return false;
  }

  const Inst call = function->Get(call_id);
  const Function& callee = module.functions[call.payload.callee];
  const bool returns_value =
      call.type.is_valid() && types->KindOf(call.type) != TypeKind::kVoid;

  // The continuation: whatever followed the call in its block. The call
  // instruction becomes its parameter, so nothing that used the result has
  // to be rewritten.
  const BlockId continuation{
      static_cast<uint32_t>(function->blocks.size())};
  Block tail;
  tail.label = "inline.continue";
  {
    Block& host = function->Get(host_id);
    const std::vector<InstId> rest(host.instructions.begin() +
                                       static_cast<long>(position) + 1,
                                   host.instructions.end());
    host.instructions.resize(position);
    if (returns_value) {
      Inst& repurposed = function->Get(call_id);
      repurposed.opcode = Opcode::kParam;
      repurposed.operands_count = 0;
      repurposed.successors_count = 0;
      repurposed.cases_count = 0;
      tail.instructions.push_back(call_id);
      tail.param_count = 1;
      function->owners[call_id.index] = continuation;
    } else {
      Discard(function, call_id);
    }
    for (InstId id : rest) {
      tail.instructions.push_back(id);
      function->owners[id.index] = continuation;
    }
  }
  function->blocks.push_back(std::move(tail));

  // A block the callee had emptied is a deleted block, and nothing in it
  // needs a copy.
  std::vector<uint32_t> block_map(callee.blocks.size(), kNoBlock);
  for (uint32_t b = 0; b < callee.blocks.size(); ++b) {
    if (callee.blocks[b].instructions.empty()) {
      continue;
    }
    block_map[b] = static_cast<uint32_t>(function->blocks.size());
    Block copy;
    copy.label = "inline." + callee.blocks[b].label;
    copy.param_count = callee.blocks[b].param_count;
    function->blocks.push_back(std::move(copy));
  }
  if (block_map[callee.entry().index] == kNoBlock) {
    return false;
  }

  // Ids first, bodies second: an operand may name an instruction that
  // comes later in the numbering.
  std::vector<InstId> value_map(callee.instructions.size(), InstId{});
  for (uint32_t b = 0; b < callee.blocks.size(); ++b) {
    if (block_map[b] == kNoBlock) {
      continue;
    }
    for (InstId id : callee.blocks[b].instructions) {
      value_map[id.index] =
          InstId{static_cast<uint32_t>(function->instructions.size())};
      function->instructions.push_back(Inst{});
      function->owners.push_back(BlockId{block_map[b]});
      function->Get(BlockId{block_map[b]}).instructions.push_back(
          value_map[id.index]);
    }
  }

  const auto map_value = [&value_map](InstId id) {
    return id.is_valid() && id.index < value_map.size() ? value_map[id.index]
                                                        : InstId{};
  };

  for (uint32_t b = 0; b < callee.blocks.size(); ++b) {
    if (block_map[b] == kNoBlock) {
      continue;
    }
    for (InstId id : callee.blocks[b].instructions) {
      const Inst& source = callee.Get(id);
      Inst copy;
      copy.opcode = source.opcode;
      copy.type = source.type;
      copy.field = source.field;
      copy.payload = source.payload;

      // A return leaves the callee, and what it leaves to is the
      // continuation.
      if (source.opcode == Opcode::kReturn) {
        copy.opcode = Opcode::kJump;
        if (returns_value && source.operands_count != 0) {
          copy.operands_begin =
              static_cast<uint32_t>(function->operands.size());
          copy.operands_count = 1;
          function->operands.push_back(
              map_value(callee.OperandsOf(source)[0]));
        }
        copy.successors_begin =
            static_cast<uint32_t>(function->successors.size());
        copy.successors_count = 1;
        function->successors.push_back(continuation);
        function->instructions[value_map[id.index].index] = copy;
        continue;
      }

      copy.operands_begin =
          static_cast<uint32_t>(function->operands.size());
      copy.operands_count = source.operands_count;
      for (InstId operand : callee.OperandsOf(source)) {
        function->operands.push_back(map_value(operand));
      }
      copy.successors_begin =
          static_cast<uint32_t>(function->successors.size());
      copy.successors_count = source.successors_count;
      for (BlockId successor : callee.SuccessorsOf(source)) {
        const uint32_t mapped =
            successor.index < block_map.size() ? block_map[successor.index]
                                              : kNoBlock;
        function->successors.push_back(BlockId{mapped});
      }
      copy.cases_begin = static_cast<uint32_t>(function->cases.size());
      copy.cases_count = source.cases_count;
      for (uint64_t value : callee.CasesOf(source)) {
        function->cases.push_back(value);
      }
      function->instructions[value_map[id.index].index] = copy;
    }
  }

  // The host block now jumps into the copied body, passing the arguments
  // the call carried. The callee's entry keeps its parameters, so the
  // arguments arrive exactly the way they do on any other edge -- which is
  // the second thing block parameters make easy.
  Inst jump;
  jump.opcode = Opcode::kJump;
  jump.operands_begin = call.operands_begin;
  jump.operands_count = call.operands_count;
  jump.successors_begin = static_cast<uint32_t>(function->successors.size());
  jump.successors_count = 1;
  function->successors.push_back(BlockId{block_map[callee.entry().index]});
  Append(function, jump, host_id);
  return true;
}

// --------------------------------------------------------- slot promotion ---

namespace {

// Reachable blocks, with a block before everything it reaches except across
// a back edge.
std::vector<BlockId> ReversePostOrder(const Function& function) {
  std::vector<bool> seen(function.blocks.size(), false);
  std::vector<BlockId> postorder;
  std::vector<std::pair<BlockId, size_t>> stack;
  stack.emplace_back(function.entry(), 0);
  seen[function.entry().index] = true;
  while (!stack.empty()) {
    auto& [block, next] = stack.back();
    const InstId terminator = function.TerminatorOf(block);
    const std::span<const BlockId> successors =
        function.Contains(terminator)
            ? function.SuccessorsOf(function.Get(terminator))
            : std::span<const BlockId>();
    if (next < successors.size()) {
      const BlockId successor = successors[next++];
      if (function.Contains(successor) && !seen[successor.index] &&
          !function.Get(successor).instructions.empty()) {
        seen[successor.index] = true;
        stack.emplace_back(successor, 0);
      }
      continue;
    }
    postorder.push_back(block);
    stack.pop_back();
  }
  return std::vector<BlockId>(postorder.rbegin(), postorder.rend());
}

// Adds |value| to the end of |inst|'s operand list. The list lives in a
// flat pool, so growing one means moving it -- which is fine, because
// nothing refers to a range by anything but the instruction that owns it.
void AppendOperand(Function* function, InstId id, InstId value) {
  Inst& inst = function->Get(id);
  const std::vector<InstId> existing(
      function->operands.begin() + inst.operands_begin,
      function->operands.begin() + inst.operands_begin +
          inst.operands_count);
  inst.operands_begin = static_cast<uint32_t>(function->operands.size());
  for (InstId operand : existing) {
    function->operands.push_back(operand);
  }
  function->operands.push_back(value);
  inst.operands_count = static_cast<uint32_t>(existing.size() + 1);
}

void EraseFromBlock(Function* function, InstId id) {
  Block& block = function->Get(function->owners[id.index]);
  const auto position =
      std::find(block.instructions.begin(), block.instructions.end(), id);
  if (position != block.instructions.end()) {
    block.instructions.erase(position);
  }
}

// Promotes one slot. Returns false without touching anything when it
// cannot, which is what keeps a bail-out from leaving half a rewrite
// behind.
bool PromoteOne(Function* function,
                InstId slot,
                const std::vector<InstId>& loads,
                const std::vector<InstId>& stores) {
  const size_t count = function->blocks.size();
  const std::vector<BlockId> order = ReversePostOrder(*function);
  std::vector<uint32_t> position(count, 0xFFFFFFFFu);
  for (uint32_t i = 0; i < order.size(); ++i) {
    position[order[i].index] = i;
  }

  // Every access has to be somewhere control reaches, or the value flowing
  // into it is not a question with an answer.
  const auto unreachable = [&](InstId access) {
    const BlockId owner = function->owners[access.index];
    return !function->Contains(owner) ||
           position[owner.index] == 0xFFFFFFFFu;
  };
  for (InstId access : loads) {
    if (unreachable(access)) {
      return false;
    }
  }
  for (InstId access : stores) {
    if (unreachable(access)) {
      return false;
    }
  }

  std::vector<std::vector<BlockId>> predecessors(count);
  for (BlockId block : order) {
    const InstId terminator = function->TerminatorOf(block);
    if (!function->Contains(terminator)) {
      continue;
    }
    for (BlockId successor :
         function->SuccessorsOf(function->Get(terminator))) {
      if (function->Contains(successor) &&
          position[successor.index] != 0xFFFFFFFFu) {
        predecessors[successor.index].push_back(block);
      }
    }
  }

  // Which blocks need a value on the way in: one that loads before it
  // stores, or one that lets a load further on see what came in.
  std::vector<bool> loads_first(count, false);
  std::vector<bool> stores_here(count, false);
  for (BlockId block : order) {
    bool stored = false;
    for (InstId id : function->Get(block).instructions) {
      const Inst& inst = function->Get(id);
      const std::span<const InstId> operands = function->OperandsOf(inst);
      const bool touches =
          !operands.empty() && operands[0] == slot;
      if (!touches) {
        continue;
      }
      if (inst.opcode == Opcode::kLoad && !stored) {
        loads_first[block.index] = true;
      }
      if (inst.opcode == Opcode::kStore) {
        stored = true;
        stores_here[block.index] = true;
      }
    }
  }

  std::vector<bool> needs_in = loads_first;
  for (int round = 0; round < static_cast<int>(count) + 2; ++round) {
    bool grew = false;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const BlockId block = *it;
      if (needs_in[block.index] || stores_here[block.index]) {
        continue;
      }
      const InstId terminator = function->TerminatorOf(block);
      if (!function->Contains(terminator)) {
        continue;
      }
      for (BlockId successor :
           function->SuccessorsOf(function->Get(terminator))) {
        if (function->Contains(successor) &&
            position[successor.index] != 0xFFFFFFFFu &&
            needs_in[successor.index]) {
          needs_in[block.index] = true;
          grew = true;
          break;
        }
      }
    }
    if (!grew) {
      break;
    }
  }

  // A load the entry can reach with no store before it reads what the slot
  // started as, which is zeroes -- in the interpreter and in the compiled
  // tiers alike. There is nothing to promote such a slot to, so it stays a
  // slot and the load stays a load.
  //
  // Substituting an undefined value here was right while "read before
  // written" meant nothing in particular. It is wrong now, and what it did
  // was turn a buffer that a callee fills into an undefined value before
  // the callee ever saw it -- which is how a server came to answer
  // nothing at all.
  if (needs_in[function->entry().index]) {
    return false;
  }

  // A block that needs a value and cannot simply take its one
  // predecessor's gets a parameter.
  std::vector<bool> wants_param(count, false);
  for (BlockId block : order) {
    if (!needs_in[block.index] || block == function->entry()) {
      continue;
    }
    const std::vector<BlockId>& preds = predecessors[block.index];
    const bool single_earlier =
        preds.size() == 1 &&
        position[preds.front().index] < position[block.index];
    wants_param[block.index] = !single_earlier;
  }

  // Only a jump can carry arguments. A branch or a switch into a block that
  // needs a parameter would have to be split first, which this pass does
  // not do -- so it declines instead of producing IR the verifier rejects.
  for (BlockId block : order) {
    if (!wants_param[block.index]) {
      continue;
    }
    for (BlockId pred : predecessors[block.index]) {
      const InstId terminator = function->TerminatorOf(pred);
      if (!function->Contains(terminator) ||
          function->Get(terminator).opcode != Opcode::kJump) {
        return false;
      }
    }
  }

  const TypeId type = function->Get(slot).type;
  std::vector<InstId> parameter(count, InstId{});
  for (BlockId block : order) {
    if (!wants_param[block.index]) {
      continue;
    }
    Inst param;
    param.opcode = Opcode::kParam;
    param.type = type;
    const InstId id{static_cast<uint32_t>(function->instructions.size())};
    function->instructions.push_back(param);
    function->owners.push_back(block);
    Block& body = function->Get(block);
    body.instructions.insert(
        body.instructions.begin() + body.param_count, id);
    ++body.param_count;
    parameter[block.index] = id;
  }

  // The rename walk. `current` is what the slot holds; at the start of a
  // block it is the parameter, the one predecessor's last value, or an
  // `undef` for a load that precedes every store.
  std::vector<InstId> leaving(count, InstId{});
  for (BlockId block : order) {
    InstId current;
    if (parameter[block.index].is_valid()) {
      current = parameter[block.index];
    } else {
      const std::vector<BlockId>& preds = predecessors[block.index];
      if (preds.size() == 1) {
        current = leaving[preds.front().index];
      }
    }

    Block& body = function->Get(block);
    std::vector<InstId> dead;
    for (InstId id : body.instructions) {
      const Inst& inst = function->Get(id);
      const std::span<const InstId> operands = function->OperandsOf(inst);
      if (operands.empty() || operands[0] != slot) {
        continue;
      }
      if (inst.opcode == Opcode::kStore && operands.size() == 2) {
        current = operands[1];
        dead.push_back(id);
        continue;
      }
      if (inst.opcode != Opcode::kLoad) {
        continue;
      }
      // Every load is dominated by a store: the bail-out above is what
      // makes that true, so there is always a value to use here.
      WOLANG_CHECK(current.is_valid());
      ReplaceAllUses(function, id, current);
      dead.push_back(id);
    }
    for (InstId id : dead) {
      Discard(function, id);
      EraseFromBlock(function, id);
    }
    leaving[block.index] = current;
  }

  // Every edge into a block that got a parameter now carries the value its
  // predecessor was left holding.
  for (BlockId block : order) {
    if (!parameter[block.index].is_valid()) {
      continue;
    }
    for (BlockId pred : predecessors[block.index]) {
      const InstId terminator = function->TerminatorOf(pred);
      const InstId value = leaving[pred.index];
      // Same reason: a path that never wrote the slot would have stopped
      // this promotion before it started.
      WOLANG_CHECK(value.is_valid());
      AppendOperand(function, terminator, value);
    }
  }
  return true;
}

}  // namespace

bool PromoteSlots(TypeStore* types, Function* function) {
  static_cast<void>(types);
  if (function->is_declaration()) {
    return false;
  }
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
    if (!only_loads_and_stores || (loads.empty() && stores.empty())) {
      continue;
    }

    // The general case: a slot written on several paths becomes a value
    // passed between blocks, which is what block parameters are for. A loop
    // counter lives in a register afterwards instead of making a round trip
    // through the frame every iteration -- the difference between ten
    // instructions an iteration and three.
    if (PromoteOne(function, slot, loads, stores)) {
      changed = true;
    }
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

  // A jump to a block only this block reaches is not a branch at all: the
  // two are one straight line. Merging them is what makes inlining pay --
  // the splice leaves a jump into the body and a jump back out, and both
  // disappear here.
  for (int round = 0; round < 16; ++round) {
    bool merged = false;
    for (uint32_t i = 0; i < function->blocks.size() && !merged; ++i) {
      const BlockId host{i};
      if (function->Get(host).instructions.empty()) {
        continue;
      }
      const InstId terminator = function->TerminatorOf(host);
      if (!function->Contains(terminator)) {
        continue;
      }
      const Inst jump = function->Get(terminator);
      if (jump.opcode != Opcode::kJump || jump.successors_count != 1) {
        continue;
      }
      const BlockId target = function->successors[jump.successors_begin];
      if (!function->Contains(target) || target == host ||
          target == function->entry() ||
          function->Get(target).instructions.empty()) {
        continue;
      }
      if (EdgesInto(*function, target) != 1) {
        continue;
      }

      // The arguments the jump carried become the values the parameters
      // stood for. Substituting them is what lets a block with parameters
      // be merged at all.
      const std::span<const InstId> params = function->ParamsOf(target);
      if (params.size() != jump.operands_count) {
        continue;
      }
      for (size_t p = 0; p < params.size(); ++p) {
        ReplaceAllUses(function, params[p],
                       function->operands[jump.operands_begin + p]);
        Discard(function, params[p]);
      }

      Block& into = function->Get(host);
      into.instructions.pop_back();
      Discard(function, terminator);
      const std::vector<InstId> moved(
          function->Get(target).instructions.begin() +
              static_cast<long>(params.size()),
          function->Get(target).instructions.end());
      for (InstId id : moved) {
        into.instructions.push_back(id);
        function->owners[id.index] = host;
      }
      function->Get(target).instructions.clear();
      function->Get(target).param_count = 0;
      merged = true;
      changed = true;
    }
    if (!merged) {
      break;
    }
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

// ------------------------------------------- common subexpressions ---

// One copy of each computation per block.
//
// A loop body that reads `bodies[i].x`, adds to it and writes it back
// works out that address twice; so does every pair of a read and a write
// to the same place. The saving is small per instruction and the loops
// this language is for run it a million times.
bool EliminateCommonSubexpressions(TypeStore* types, Function* function) {
  (void)types;
  bool changed = false;
  for (Block& block : function->blocks) {
    // Earlier instructions of this block that something later may reuse.
    std::vector<InstId> available;
    std::vector<InstId> dead;
    for (size_t position = block.param_count;
         position < block.instructions.size(); ++position) {
      const InstId id = block.instructions[position];
      if (!function->Contains(id)) {
        continue;
      }
      const Inst& inst = function->Get(id);
      // A write, or a call that might write: nothing read before it can be
      // reused after it, because what is at an address may have changed.
      // Everything worked out *from* a value stays valid, so only the
      // loads are forgotten.
      if (inst.opcode == Opcode::kStore || inst.opcode == Opcode::kCall ||
          inst.opcode == Opcode::kCallIndirect ||
          inst.opcode == Opcode::kWaitCall ||
          inst.opcode == Opcode::kSpawnCall) {
        available.erase(
            std::remove_if(available.begin(), available.end(),
                           [&](InstId earlier) {
                             return function->Contains(earlier) &&
                                    function->Get(earlier).opcode ==
                                        Opcode::kLoad;
                           }),
            available.end());
        continue;
      }
      // A load is repeatable between writes: reading the same address
      // twice with nothing written in between gives the same answer, and
      // a loop that reads a field of its object pays for it once instead
      // of once per use.
      if (!IsRepeatable(inst.opcode) && inst.opcode != Opcode::kLoad) {
        continue;
      }
      bool replaced = false;
      for (InstId earlier : available) {
        if (!function->Contains(earlier) ||
            !SameComputation(*function, function->Get(earlier), inst)) {
          continue;
        }
        ReplaceAllUses(function, id, earlier);
        dead.push_back(id);
        replaced = true;
        changed = true;
        break;
      }
      if (!replaced) {
        available.push_back(id);
      }
    }
    for (InstId id : dead) {
      Discard(function, id);
      EraseFromBlock(function, id);
    }
  }
  return changed;
}

// ------------------------------------------------ loop invariants ---

// Moves what does not change out of the loop that computes it.
//
// The pass that matters most for the loops this language is for. An
// address inside `bodies[i].x` is the base of a field's array -- the same
// pointer every time round -- plus this element; expanding the address
// (above) made those pieces visible, and this is what moves the fixed ones
// out so the loop does a multiply and an add.
//
// Only what cannot trap is moved: hoisting a division out of a loop that
// never runs would trap where the program would not have. Arithmetic the
// compiler wrote for itself is not checked, so it qualifies -- which is
// the other half of why addresses are expanded into it.
bool HoistLoopInvariants(TypeStore* types, Function* function) {
  (void)types;
  const LinearOrder order = Linearize(*function);
  if (order.blocks.size() < 2) {
    return false;
  }
  const size_t count = function->blocks.size();

  // Where each block sits in the order, so that a jump backwards in it can
  // be recognised: in reverse postorder a block comes after everything
  // that reaches it except through a back edge, which is what a loop is.
  std::vector<uint32_t> position(count, 0);
  for (size_t i = 0; i < order.blocks.size(); ++i) {
    position[order.blocks[i].index] = static_cast<uint32_t>(i) + 1;
  }

  const auto terminator = [&](BlockId block) -> const Inst* {
    const Block& body = function->blocks[block.index];
    if (body.instructions.empty()) {
      return nullptr;
    }
    const InstId last = body.instructions.back();
    return function->Contains(last) ? &function->Get(last) : nullptr;
  };

  std::vector<std::vector<BlockId>> predecessors(count);
  for (BlockId block : order.blocks) {
    const Inst* jump = terminator(block);
    if (jump == nullptr) {
      continue;
    }
    for (BlockId successor : function->SuccessorsOf(*jump)) {
      if (successor.is_valid() && position[successor.index] != 0) {
        predecessors[successor.index].push_back(block);
      }
    }
  }

  // Immediate dominators, by the usual iteration over reverse postorder:
  // a block's is the one every way in has in common, found by walking two
  // chains until they meet. Kept as a chain rather than as a matrix of who
  // dominates whom -- the matrix is what made this pass cost minutes on a
  // function with a few hundred blocks.
  std::vector<uint32_t> idom(count, 0);
  const BlockId entry = order.blocks.front();
  idom[entry.index] = position[entry.index];
  // Where two chains come together. Zero when one of them does not reach
  // the entry yet, which happens on the first round for a block whose
  // only other way in is a back edge -- and walking a chain that is not
  // there yet is how this looped forever on one of the two back ends.
  const auto meet = [&](uint32_t left, uint32_t right) -> uint32_t {
    while (left != right) {
      while (left > right) {
        const uint32_t next = idom[order.blocks[left - 1].index];
        if (next == 0 || next == left) {
          return 0;
        }
        left = next;
      }
      while (right > left) {
        const uint32_t next = idom[order.blocks[right - 1].index];
        if (next == 0 || next == right) {
          return 0;
        }
        right = next;
      }
    }
    return left;
  };
  bool settled = false;
  while (!settled) {
    settled = true;
    for (size_t i = 1; i < order.blocks.size(); ++i) {
      const BlockId block = order.blocks[i];
      uint32_t candidate = 0;
      for (BlockId predecessor : predecessors[block.index]) {
        if (idom[predecessor.index] == 0) {
          continue;
        }
        const uint32_t merged =
            candidate == 0 ? position[predecessor.index]
                           : meet(candidate, position[predecessor.index]);
        if (merged == 0) {
          // Nothing certain this round; the next one will have more.
          candidate = 0;
          break;
        }
        candidate = merged;
      }
      if (candidate != 0 && candidate != idom[block.index]) {
        idom[block.index] = candidate;
        settled = false;
      }
    }
  }
  const auto dominates = [&](BlockId above, BlockId below) {
    uint32_t walk = position[below.index];
    while (walk != 0) {
      if (walk == position[above.index]) {
        return true;
      }
      const uint32_t next = idom[order.blocks[walk - 1].index];
      if (next == walk) {
        return false;
      }
      walk = next;
    }
    return false;
  };

  bool changed = false;
  for (size_t i = 0; i < order.blocks.size(); ++i) {
    const BlockId tail = order.blocks[i];
    const Inst* jump = terminator(tail);
    if (jump == nullptr) {
      continue;
    }
    for (BlockId header : function->SuccessorsOf(*jump)) {
      if (!header.is_valid() || position[header.index] == 0 ||
          position[header.index] > position[tail.index] ||
          !dominates(header, tail)) {
        continue;
      }

      // The loop: the header, and everything that reaches the tail
      // without going back through the header. Walked backwards over the
      // predecessors, so it costs one visit per edge.
      std::vector<bool> inside(count, false);
      inside[header.index] = true;
      std::vector<BlockId> pending;
      if (tail != header) {
        inside[tail.index] = true;
        pending.push_back(tail);
      }
      while (!pending.empty()) {
        const BlockId block = pending.back();
        pending.pop_back();
        for (BlockId predecessor : predecessors[block.index]) {
          // Only blocks the header runs before: anything else is entered
          // some other way, and what is hoisted would not have run.
          if (inside[predecessor.index] ||
              !dominates(header, predecessor)) {
            continue;
          }
          inside[predecessor.index] = true;
          pending.push_back(predecessor);
        }
      }

      // Somewhere to put what is hoisted: the one way into the loop from
      // outside, which also has to run before it. A resumed task is
      // entered by a jump table that lands in the middle of its loops, so
      // "the only way in" and "always runs before" are two questions.
      BlockId preheader;
      bool ambiguous = false;
      for (BlockId candidate : predecessors[header.index]) {
        if (inside[candidate.index]) {
          continue;
        }
        if (preheader.is_valid() && preheader != candidate) {
          ambiguous = true;
        }
        preheader = candidate;
      }
      if (!preheader.is_valid() || ambiguous ||
          !dominates(preheader, header)) {
        continue;
      }

      for (BlockId block : order.blocks) {
        if (!inside[block.index]) {
          continue;
        }
        Block& body = function->blocks[block.index];
        const std::vector<InstId> instructions = body.instructions;
        for (size_t at = body.param_count; at < instructions.size(); ++at) {
          const InstId id = instructions[at];
          if (!function->Contains(id)) {
            continue;
          }
          const Inst& inst = function->Get(id);
          if (IsTerminator(inst.opcode) || !IsRepeatable(inst.opcode) ||
              !(inst.is_unchecked || IsPure(inst.opcode))) {
            continue;
          }
          bool invariant = true;
          for (InstId operand : function->OperandsOf(inst)) {
            if (!function->Contains(operand)) {
              continue;
            }
            const BlockId owner = function->owners[operand.index];
            if (owner.is_valid() && inside[owner.index]) {
              invariant = false;
              break;
            }
          }
          if (!invariant) {
            continue;
          }

          // Before the jump that ends the preheader, which is the last
          // point that runs once on the way in.
          Block& landing = function->blocks[preheader.index];
          if (landing.instructions.empty()) {
            continue;
          }
          EraseFromBlock(function, id);
          landing.instructions.insert(landing.instructions.end() - 1, id);
          function->owners[id.index] = preheader;
          changed = true;
        }
      }
    }
  }
  return changed;
}

// --------------------------------------------- element addresses ---

// Turns an element address into the arithmetic it is.
//
// `index.addr` and `soa.addr` are instructions so that lowering does not
// need a layout. Once the layout *is* known, keeping them opaque costs
// more than it saves: the pieces of an address -- the base, the length,
// the stride of a field's array -- are the same every time round a loop,
// and nothing can hoist what it cannot see inside.
//
// So they are expanded here, into arithmetic marked as the compiler's own
// so that it is neither checked nor pinned in place. What is left is what
// a loop actually has to do per element: one multiply and one add, with
// everything else hoisted out by the passes that follow.
bool ExpandElementAddresses(TypeStore* types,
                            abi::LayoutStore* layouts,
                            Function* function) {
  if (layouts == nullptr) {
    return false;
  }
  const TypeId word = types->Int(IntWidth::k64, /*is_signed=*/false);
  bool changed = false;

  for (Block& block : function->blocks) {
    std::vector<InstId> instructions = block.instructions;
    for (InstId id : instructions) {
      if (!function->Contains(id)) {
        continue;
      }
      // Only the one array per field. `index.addr` stays as it is: the
      // reference tier reads an array inside a slot as a path rather than
      // an address, so arithmetic is not something it could stand in for.
      const Opcode opcode = function->Get(id).opcode;
      if (opcode != Opcode::kSoaAddress) {
        continue;
      }

      // Everything is appended to this block and then moved into place
      // just before the address it replaces, so that the pieces come
      // before the use even when the block is walked again.
      std::vector<InstId> built;
      const auto emit = [&](Opcode op, bool unchecked,
                            std::initializer_list<InstId> operands) {
        Inst inst;
        inst.opcode = op;
        inst.type = word;
        inst.is_unchecked = unchecked;
        inst.operands_begin =
            static_cast<uint32_t>(function->operands.size());
        inst.operands_count = static_cast<uint32_t>(operands.size());
        for (InstId operand : operands) {
          function->operands.push_back(operand);
        }
        const InstId made =
            InstId{static_cast<uint32_t>(function->instructions.size())};
        function->instructions.push_back(inst);
        function->owners.push_back(BlockId{});
        built.push_back(made);
        return made;
      };
      const auto constant = [&](uint64_t value) {
        Inst inst;
        inst.opcode = Opcode::kConstInt;
        inst.type = word;
        inst.payload.integer = value;
        const InstId made =
            InstId{static_cast<uint32_t>(function->instructions.size())};
        function->instructions.push_back(inst);
        function->owners.push_back(BlockId{});
        built.push_back(made);
        return made;
      };

      // Copied, not a span into the function: everything below appends
      // operands, and the first append moves the storage the span points
      // at. The x86-64 build read the freed copy and looped forever; the
      // arm64 one happened to read the right bytes, which is what a
      // dangling read does.
      const std::span<const InstId> raw =
          function->OperandsOf(function->Get(id));
      const std::vector<InstId> operands(raw.begin(), raw.end());
      const InstId base = operands[0];
      const InstId index = operands[1];

      // The check stays: what is expanded here is the arithmetic around
      // it, not the question of whether the element exists.
      {
        Inst check;
        check.opcode = Opcode::kIndexCheck;
        check.operands_begin =
            static_cast<uint32_t>(function->operands.size());
        check.operands_count = 2;
        function->operands.push_back(index);
        function->operands.push_back(operands[2]);
        const InstId made =
            InstId{static_cast<uint32_t>(function->instructions.size())};
        function->instructions.push_back(check);
        function->owners.push_back(BlockId{});
        built.push_back(made);
      }

      InstId offset;
      {
        // One array per field: the fields before this one, times the
        // padded count, plus this element.
        const InstId count = operands[2];
        const TypeId record =
            SoaRecordOf(*types, *function, function->Get(id));
        const uint32_t field = function->Get(id).field;
        const TypeId element = layouts->FieldType(record, field);
        const uint64_t size =
            element.is_valid() ? layouts->SizeOf(element) : 0;
        const InstId padded = emit(
            Opcode::kBitAnd, true,
            {emit(Opcode::kAdd, true, {count, constant(15)}),
             constant(~uint64_t{15})});
        const InstId blocks = emit(
            Opcode::kMul, true,
            {padded, constant(layouts->SoaPrefixOf(record, field))});
        // Grouped so that everything about the *field* is on one side and
        // everything about the *element* on the other: `(base + array) +
        // index * size` leaves a loop with one add per field, because the
        // start of the field's array does not change and is hoisted out.
        // Grouped the other way -- which is how this read first -- the
        // base is added inside the loop four times for four fields.
        offset = emit(Opcode::kAdd, true, {base, blocks});
        offset = emit(Opcode::kAdd, true,
                      {offset, emit(Opcode::kMul, true,
                                    {index, constant(size)})});
      }
      const InstId address = offset;
      function->instructions[address.index].type = function->Get(id).type;

      // In place, in order, where the address used to be.
      const auto at = std::find(block.instructions.begin(),
                                block.instructions.end(), id);
      const size_t position =
          static_cast<size_t>(at - block.instructions.begin());
      for (InstId made : built) {
        function->owners[made.index] = BlockId{
            static_cast<uint32_t>(&block - function->blocks.data())};
      }
      block.instructions.insert(block.instructions.begin() + position,
                                built.begin(), built.end());
      ReplaceAllUses(function, id, address);
      Discard(function, id);
      EraseFromBlock(function, id);
      changed = true;
    }
  }
  return changed;
}

void Optimize(TypeStore* types,
              const Module& module,
              Function* function,
              abi::LayoutStore* layouts) {
  if (function->is_declaration()) {
    return;
  }
  // Before the rest: an address the passes can see inside is an address
  // they can hoist out of a loop.
  ExpandElementAddresses(types, layouts, function);
  // To a fixpoint, bounded so that a pass that keeps reporting a change
  // cannot hang the compiler.
  for (int round = 0; round < 8; ++round) {
    bool changed = false;
    changed |= InlineCalls(types, module, function);
    changed |= FoldConstants(types, function);
    changed |= SimplifyControlFlow(types, function);
    changed |= PromoteSlots(types, function);
    changed |= EliminateCommonSubexpressions(types, function);
    changed |= HoistLoopInvariants(types, function);
    changed |= EliminateDeadCode(types, module, function);
    if (!changed) {
      return;
    }
  }
}

void Optimize(TypeStore* types, Module* module, abi::LayoutStore* layouts) {
  for (Function& function : module->functions) {
    Optimize(types, *module, &function, layouts);
  }
}

}  // namespace ir
}  // namespace wolang
