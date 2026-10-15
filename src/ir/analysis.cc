// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/analysis.h"

#include <algorithm>

namespace wolang {
namespace ir {
namespace {

// Depth first postorder, reversed. A block nothing jumps to is left out
// entirely, which is also how a pass marks a block deleted.
void Visit(const Function& function,
           BlockId block,
           std::vector<bool>* seen,
           std::vector<BlockId>* postorder) {
  if (!function.Contains(block) || (*seen)[block.index]) {
    return;
  }
  (*seen)[block.index] = true;
  const InstId terminator = function.TerminatorOf(block);
  if (terminator.is_valid()) {
    const Inst& inst = function.Get(terminator);
    for (BlockId successor : function.SuccessorsOf(inst)) {
      Visit(function, successor, seen, postorder);
    }
  }
  postorder->push_back(block);
}

}  // namespace

LinearOrder Linearize(const Function& function) {
  LinearOrder order;
  order.positions.assign(function.instructions.size(), 0);
  order.block_begin.assign(function.blocks.size(), 0);
  order.block_end.assign(function.blocks.size(), 0);
  if (function.is_declaration()) {
    return order;
  }

  std::vector<bool> seen(function.blocks.size(), false);
  std::vector<BlockId> postorder;
  Visit(function, function.entry(), &seen, &postorder);
  order.blocks.assign(postorder.rbegin(), postorder.rend());

  uint32_t position = 1;
  for (BlockId block : order.blocks) {
    order.block_begin[block.index] = position;
    for (InstId id : function.Get(block).instructions) {
      order.positions[id.index] = position;
      ++position;
    }
    order.block_end[block.index] = position;
  }
  return order;
}

std::vector<LiveRange> ComputeLiveRanges(const Function& function,
                                         const LinearOrder& order,
                                         const std::vector<bool>& tracked) {
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
      const BlockId block = *it;
      const Block& body = function.Get(block);
      std::vector<bool>& out = live_out[block.index];

      const InstId terminator = function.TerminatorOf(block);
      if (terminator.is_valid()) {
        for (BlockId successor :
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
        const Inst& described = function.Get(*inst);
        for (InstId operand : function.OperandsOf(described)) {
          if (operand.is_valid() && operand.index < value_count) {
            live[operand.index] = true;
          }
        }
      }
      // The parameters are defined here, not live in.
      for (InstId param : function.ParamsOf(block)) {
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

  for (BlockId block : order.blocks) {
    const Block& body = function.Get(block);
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

    for (InstId id : body.instructions) {
      const uint32_t position = order.positions[id.index];
      if (start[id.index] == 0) {
        start[id.index] = position;
      }
      end[id.index] = std::max(end[id.index], position + 1);
      for (InstId operand : function.OperandsOf(function.Get(id))) {
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

  std::vector<LiveRange> ranges;
  for (size_t v = 0; v < value_count; ++v) {
    if (v >= tracked.size() || !tracked[v] || start[v] == 0) {
      continue;
    }
    ranges.push_back(LiveRange{static_cast<uint32_t>(v), start[v],
                               std::max(end[v], start[v] + 1)});
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const LiveRange& lhs, const LiveRange& rhs) {
              return lhs.start != rhs.start ? lhs.start < rhs.start
                                            : lhs.value < rhs.value;
            });
  return ranges;
}

}  // namespace ir
}  // namespace wolang
