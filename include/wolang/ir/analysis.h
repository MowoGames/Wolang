// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_IR_ANALYSIS_H_
#define WOLANG_IR_ANALYSIS_H_

#include <cstdint>
#include <vector>

#include "wolang/ir/ir.h"

namespace wolang {
namespace ir {

// Analyses over a function that more than one consumer needs: the register
// allocator wants to know where a value's life ends, and the transform that
// splits a `task` into a state machine wants to know which values are alive
// across a suspension point. Both questions are the same question.

// The blocks in the order they will be emitted, with every instruction given
// a position. Everything downstream is in terms of these positions: "is this
// value still needed" becomes a number comparison.
struct LinearOrder {
  std::vector<BlockId> blocks;
  // Indexed by instruction id. Positions start at one, so zero reads as
  // "not in the order" -- an instruction in an unreachable block.
  std::vector<uint32_t> positions;
  // Indexed by block id.
  std::vector<uint32_t> block_begin;
  std::vector<uint32_t> block_end;

  bool Reaches(BlockId block) const {
    return block.is_valid() && block.index < block_begin.size() &&
           block_begin[block.index] != 0;
  }
};

// Reachable blocks in reverse postorder, which puts a block after everything
// that can jump to it except a back edge.
LinearOrder Linearize(const Function& function);

// One value's life, as the positions between its definition and its last
// use. One interval per value rather than a list of ranges with holes: a
// value that is dead in the middle of its life is treated as live anyway,
// which costs some precision and removes a family of bugs.
struct LiveRange {
  uint32_t value = 0;
  uint32_t start = 0;
  // Exclusive. A value's range ends one past its last use so that the
  // result of an instruction never shares a register with an operand of it:
  // an overflow check reads its operands again *after* writing the result.
  uint32_t end = 0;
};

// The live range of every value |tracked| marks, indexed by instruction id.
std::vector<LiveRange> ComputeLiveRanges(const Function& function,
                                         const LinearOrder& order,
                                         const std::vector<bool>& tracked);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_ANALYSIS_H_
