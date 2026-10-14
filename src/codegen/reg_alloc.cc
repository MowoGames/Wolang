// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/reg_alloc.h"

#include <algorithm>

#include "wolang/base/check.h"

namespace wolang {
namespace codegen {

std::vector<Interval> ComputeIntervals(const ir::Function& function,
                                       const ir::LinearOrder& order,
                                       const std::vector<RegClass>& classes) {
  std::vector<bool> tracked(classes.size(), false);
  for (size_t i = 0; i < classes.size(); ++i) {
    tracked[i] = classes[i] != RegClass::kNone;
  }
  std::vector<Interval> intervals;
  for (const ir::LiveRange& range :
       ir::ComputeLiveRanges(function, order, tracked)) {
    intervals.push_back(Interval{range.value, range.start, range.end,
                                 classes[range.value]});
  }
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
