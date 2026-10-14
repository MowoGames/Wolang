// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/reg_alloc.h"

#include <algorithm>

#include "wolang/base/check.h"

namespace wolang {
namespace codegen {
namespace {

// Depth first postorder, reversed. A block nothing jumps to is left out
// entirely, which is also how a pass marks a block deleted.
void Visit(const ir::Function& function,
           ir::BlockId block,
           std::vector<bool>* seen,
           std::vector<ir::BlockId>* postorder) {
  if (!function.Contains(block) || (*seen)[block.index]) {
    return;
  }
  (*seen)[block.index] = true;
  const ir::InstId terminator = function.TerminatorOf(block);
  if (terminator.is_valid()) {
    const ir::Inst& inst = function.Get(terminator);
    for (ir::BlockId successor : function.SuccessorsOf(inst)) {
      Visit(function, successor, seen, postorder);
    }
  }
  postorder->push_back(block);
}

}  // namespace

LinearOrder Linearize(const ir::Function& function) {
  LinearOrder order;
  order.positions.assign(function.instructions.size(), 0);
  order.block_begin.assign(function.blocks.size(), 0);
  order.block_end.assign(function.blocks.size(), 0);
  if (function.is_declaration()) {
    return order;
  }

  std::vector<bool> seen(function.blocks.size(), false);
  std::vector<ir::BlockId> postorder;
  Visit(function, function.entry(), &seen, &postorder);
  order.blocks.assign(postorder.rbegin(), postorder.rend());

  uint32_t position = 1;
  for (ir::BlockId block : order.blocks) {
    order.block_begin[block.index] = position;
    for (ir::InstId id : function.Get(block).instructions) {
      order.positions[id.index] = position;
      ++position;
    }
    order.block_end[block.index] = position;
  }
  return order;
}

std::vector<Interval> ComputeIntervals(const ir::Function& function,
                                       const LinearOrder& order,
                                       const std::vector<RegClass>& classes) {
  const size_t block_count = function.blocks.size();
  const size_t value_count = function.instructions.size();

  // Liveness, the usual backward fixpoint. A jump's operands are uses in
  // the block that jumps, and the target's parameters are definitions in
  // the target -- which is exactly why block parameters are easier to get
  // right here than phi nodes, whose operands belong to neither block.
  std::vector<std::vector<bool>> live_in(block_count,
                                         std::vector<bool>(value_count,
                                                           false));
  std::vector<std::vector<bool>> live_out(block_count,
                                          std::vector<bool>(value_count,
                                                            false));

  bool changed = true;
  while (changed) {
    changed = false;
    for (auto it = order.blocks.rbegin(); it != order.blocks.rend(); ++it) {
      const ir::BlockId block = *it;
      const ir::Block& body = function.Get(block);
      std::vector<bool>& out = live_out[block.index];

      const ir::InstId terminator = function.TerminatorOf(block);
      if (terminator.is_valid()) {
        for (ir::BlockId successor :
             function.SuccessorsOf(function.Get(terminator))) {
          if (!function.Contains(successor)) {
            continue;
          }
          const std::vector<bool>& successor_in = live_in[successor.index];
          for (size_t v = 0; v < value_count; ++v) {
            if (successor_in[v] && !out[v]) {
              out[v] = true;
              changed = true;
            }
          }
        }
      }

      // Walk the block backwards: a value is live at the top if it is used
      // before it is defined here.
      std::vector<bool> live = out;
      for (auto inst = body.instructions.rbegin();
           inst != body.instructions.rend(); ++inst) {
        live[inst->index] = false;
        const ir::Inst& described = function.Get(*inst);
        for (ir::InstId operand : function.OperandsOf(described)) {
          if (operand.is_valid() && operand.index < value_count) {
            live[operand.index] = true;
          }
        }
      }
      // The parameters are defined here, not live in.
      for (ir::InstId param : function.ParamsOf(block)) {
        live[param.index] = false;
      }
      std::vector<bool>& in = live_in[block.index];
      for (size_t v = 0; v < value_count; ++v) {
        if (live[v] && !in[v]) {
          in[v] = true;
          changed = true;
        }
      }
    }
  }

  std::vector<uint32_t> start(value_count, 0);
  std::vector<uint32_t> end(value_count, 0);

  for (ir::BlockId block : order.blocks) {
    const ir::Block& body = function.Get(block);
    const uint32_t begin = order.block_begin[block.index];
    const uint32_t finish = order.block_end[block.index];

    for (size_t v = 0; v < value_count; ++v) {
      if (live_in[block.index][v]) {
        if (start[v] == 0) {
          start[v] = begin;
        }
        end[v] = std::max(end[v], begin);
      }
      if (live_out[block.index][v]) {
        // Live across the bottom edge, so the value has to survive to the
        // end of the block -- including a back edge, where the block it
        // reaches has already been numbered.
        end[v] = std::max(end[v], finish);
        if (start[v] == 0) {
          start[v] = begin;
        }
      }
    }

    for (ir::InstId id : body.instructions) {
      const uint32_t position = order.positions[id.index];
      if (start[id.index] == 0) {
        start[id.index] = position;
      }
      end[id.index] = std::max(end[id.index], position + 1);
      for (ir::InstId operand : function.OperandsOf(function.Get(id))) {
        if (!operand.is_valid() || operand.index >= value_count) {
          continue;
        }
        if (start[operand.index] == 0) {
          start[operand.index] = position;
        }
        end[operand.index] = std::max(end[operand.index], position + 1);
      }
    }
  }

  std::vector<Interval> intervals;
  for (size_t v = 0; v < value_count; ++v) {
    const RegClass reg_class =
        v < classes.size() ? classes[v] : RegClass::kNone;
    if (reg_class == RegClass::kNone || start[v] == 0) {
      continue;
    }
    intervals.push_back(Interval{static_cast<uint32_t>(v), start[v],
                                 std::max(end[v], start[v] + 1), reg_class});
  }
  std::sort(intervals.begin(), intervals.end(),
            [](const Interval& lhs, const Interval& rhs) {
              return lhs.start != rhs.start ? lhs.start < rhs.start
                                            : lhs.value < rhs.value;
            });
  return intervals;
}

namespace {

// One register file's worth of bookkeeping.
struct Pool {
  uint32_t count = 0;
  std::vector<bool> taken;
  // Values holding a register, kept sorted by the end of their interval so
  // that expiring is a walk from the front.
  std::vector<const Interval*> active;
  uint32_t used = 0;

  explicit Pool(uint32_t registers) : count(registers), taken(registers,
                                                              false) {}

  uint32_t Take() {
    for (uint32_t i = 0; i < count; ++i) {
      if (!taken[i]) {
        taken[i] = true;
        used = std::max(used, i + 1);
        return i;
      }
    }
    return Allocation::kNowhere;
  }
};

}  // namespace

AllocationResult AllocateRegisters(const std::vector<Interval>& intervals,
                                   uint32_t integer_registers,
                                   uint32_t float_registers) {
  AllocationResult result;
  uint32_t highest = 0;
  for (const Interval& interval : intervals) {
    highest = std::max(highest, interval.value);
  }
  result.values.assign(intervals.empty() ? 0 : highest + 1, Allocation{});

  Pool integers(integer_registers);
  Pool floats(float_registers);

  for (const Interval& interval : intervals) {
    Pool& pool =
        interval.reg_class == RegClass::kFloat ? floats : integers;

    // Expire everything whose life ended before this one begins.
    for (auto it = pool.active.begin(); it != pool.active.end();) {
      if ((*it)->end > interval.start) {
        ++it;
        continue;
      }
      const Allocation& held = result.values[(*it)->value];
      if (held.in_register()) {
        pool.taken[held.reg] = false;
      }
      it = pool.active.erase(it);
    }

    const uint32_t reg = pool.Take();
    if (reg != Allocation::kNowhere) {
      result.values[interval.value].reg = reg;
      pool.active.push_back(&interval);
    } else {
      // Nothing free: the value with the furthest end gives up its
      // register, because it is the one that would hold it longest.
      auto furthest = std::max_element(
          pool.active.begin(), pool.active.end(),
          [](const Interval* lhs, const Interval* rhs) {
            return lhs->end < rhs->end;
          });
      if (furthest != pool.active.end() && (*furthest)->end > interval.end) {
        Allocation& evicted = result.values[(*furthest)->value];
        result.values[interval.value].reg = evicted.reg;
        evicted.reg = Allocation::kNowhere;
        evicted.slot = result.spill_count++;
        *furthest = &interval;
      } else {
        result.values[interval.value].slot = result.spill_count++;
      }
    }
    std::sort(pool.active.begin(), pool.active.end(),
              [](const Interval* lhs, const Interval* rhs) {
                return lhs->end < rhs->end;
              });
  }

  result.integers_used = integers.used;
  result.floats_used = floats.used;
  return result;
}

std::string Describe(const std::vector<Interval>& intervals,
                     const AllocationResult& result) {
  std::string text;
  for (const Interval& interval : intervals) {
    text.append("%");
    text.append(std::to_string(interval.value));
    text.append(" [");
    text.append(std::to_string(interval.start));
    text.append(",");
    text.append(std::to_string(interval.end));
    text.append(") ");
    const Allocation& allocation = result.values[interval.value];
    if (allocation.in_register()) {
      text.append(interval.reg_class == RegClass::kFloat ? "f" : "r");
      text.append(std::to_string(allocation.reg));
    } else {
      text.append("spill");
      text.append(std::to_string(allocation.slot));
    }
    text.append("\n");
  }
  return text;
}

}  // namespace codegen
}  // namespace wolang
