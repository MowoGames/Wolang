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

  // A block that needs a value and cannot simply take its one
  // predecessor's gets a parameter. The entry takes `undef` instead: a
  // load before any store is exactly what an undefined value is.
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
  InstId undefined;
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
      if (!current.is_valid()) {
        if (!undefined.is_valid()) {
          Inst undef;
          undef.opcode = Opcode::kUndef;
          undef.type = type;
          undefined =
              InstId{static_cast<uint32_t>(function->instructions.size())};
          function->instructions.push_back(undef);
          function->owners.push_back(function->entry());
          Block& entry = function->Get(function->entry());
          entry.instructions.insert(
              entry.instructions.begin() + entry.param_count, undefined);
        }
        current = undefined;
      }
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
      InstId value = leaving[pred.index];
      if (!value.is_valid()) {
        // Nothing on this path ever wrote the slot.
        value = undefined.is_valid() ? undefined : InstId{};
        if (!value.is_valid()) {
          Inst undef;
          undef.opcode = Opcode::kUndef;
          undef.type = type;
          undefined =
              InstId{static_cast<uint32_t>(function->instructions.size())};
          function->instructions.push_back(undef);
          function->owners.push_back(function->entry());
          Block& entry = function->Get(function->entry());
          entry.instructions.insert(
              entry.instructions.begin() + entry.param_count, undefined);
          value = undefined;
        }
      }
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

void Optimize(TypeStore* types, const Module& module, Function* function) {
  if (function->is_declaration()) {
    return;
  }
  // To a fixpoint, bounded so that a pass that keeps reporting a change
  // cannot hang the compiler.
  for (int round = 0; round < 8; ++round) {
    bool changed = false;
    changed |= InlineCalls(types, module, function);
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
