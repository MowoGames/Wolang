// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_REG_ALLOC_H_
#define WOLANG_CODEGEN_REG_ALLOC_H_

#include <cstdint>
#include <string>
#include <vector>

#include "wolang/ir/ir.h"

namespace wolang {
namespace codegen {

// Which register file a value belongs in. A value the dev tier keeps in
// memory -- an aggregate -- is kNone and the allocator never sees it.
enum class RegClass : uint8_t { kNone, kInteger, kFloat };

// The blocks in the order they will be emitted, with every instruction given
// a position. Everything the allocator does is in terms of these positions:
// "this value is still needed" is a number comparison and nothing else.
struct LinearOrder {
  std::vector<ir::BlockId> blocks;
  // Indexed by instruction id. Positions start at one, so zero reads as
  // "not in the order" -- an instruction in an unreachable block.
  std::vector<uint32_t> positions;
  // Indexed by block id.
  std::vector<uint32_t> block_begin;
  std::vector<uint32_t> block_end;

  bool Reaches(ir::BlockId block) const {
    return block.is_valid() && block.index < block_begin.size() &&
           block_begin[block.index] != 0;
  }
};

// Reachable blocks in reverse postorder, which puts a block after everything
// that can jump to it except a back edge.
LinearOrder Linearize(const ir::Function& function);

// One value's life, as the positions between its definition and its last
// use. One interval per value rather than a list of ranges with holes: a
// value that is dead in the middle of its life holds its register anyway,
// which costs some spilling and removes a whole family of bugs.
struct Interval {
  uint32_t value = 0;
  uint32_t start = 0;
  // Exclusive. A value's interval ends one past its last use so that the
  // result of an instruction never shares a register with an operand of it:
  // an overflow check reads its operands again *after* writing the result.
  uint32_t end = 0;
  RegClass reg_class = RegClass::kNone;
};

// The live interval of every value whose class is not kNone. |classes| is
// indexed by instruction id.
std::vector<Interval> ComputeIntervals(const ir::Function& function,
                                       const LinearOrder& order,
                                       const std::vector<RegClass>& classes);

// Where the allocator put one value.
struct Allocation {
  static constexpr uint32_t kNowhere = 0xFFFFFFFFu;
  // An index into the class's register pool, or kNowhere when spilled.
  uint32_t reg = kNowhere;
  // A spill slot index, or kNowhere when the value is in a register.
  uint32_t slot = kNowhere;

  bool in_register() const { return reg != kNowhere; }
  bool is_spilled() const { return slot != kNowhere; }
};

struct AllocationResult {
  // Indexed by instruction id.
  std::vector<Allocation> values;
  uint32_t spill_count = 0;
  // Which registers of each class were handed out, so the prologue knows
  // what to save.
  uint32_t integers_used = 0;
  uint32_t floats_used = 0;
};

// Linear scan register allocation (Poletto and Sarkar).
//
// Chosen from the start rather than growing out of accumulator stencils
// (D39): a stencil that can only hold a value in one place has the
// allocation decision baked into it, and every stencil would have to be
// rewritten the day registers arrive. The allocator is deliberately the
// simple form -- intervals without holes, no coalescing, no live range
// splitting -- because the dev tier's job is to compile fast and be right.
AllocationResult AllocateRegisters(const std::vector<Interval>& intervals,
                                   uint32_t integer_registers,
                                   uint32_t float_registers);

// A one-line-per-value rendering, for tests and for a dump.
std::string Describe(const std::vector<Interval>& intervals,
                     const AllocationResult& result);

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_REG_ALLOC_H_
