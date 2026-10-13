// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SEMA_EFFECT_H_
#define WOLANG_SEMA_EFFECT_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wolang/base/string_interner.h"

namespace wolang {

// Identifies one effect: a built-in one, or an `effect` declaration.
struct EffectId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
  friend bool operator==(EffectId lhs, EffectId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(EffectId lhs, EffectId rhs) { return !(lhs == rhs); }
};

// The effects the language knows about without being told. Interned first, so
// their ids are these constants and a lookup table is not needed to test for
// one.
enum class BuiltinEffect : uint32_t {
  kAlloc,
  kIo,
  kRand,
  kTime,
  kNet,
  kGpu,
  kUnsafe,
  kPanic,
  kCount,
};

std::string_view BuiltinEffectName(BuiltinEffect effect);

// Returns the built-in effect spelled |text|, or kCount.
BuiltinEffect LookupBuiltinEffect(std::string_view text);

// What a function may do: a set of effects, kept sorted so that two equal
// rows compare equal and a cached row can be back-dated.
//
// A row is not the same as "unknown": an empty row is the positive claim that
// a function does nothing observable, which is what `comptime` requires.
class EffectRow {
 public:
  EffectRow() = default;

  static EffectRow Single(EffectId effect);

  bool empty() const { return effects_.empty(); }
  const std::vector<EffectId>& effects() const { return effects_; }

  bool Contains(EffectId effect) const;
  bool Contains(BuiltinEffect effect) const {
    return Contains(EffectId{static_cast<uint32_t>(effect)});
  }

  // Adds |effect|, keeping the row sorted. Returns true when the row grew,
  // which is what a fixpoint iteration watches.
  bool Add(EffectId effect);
  // Adds everything in |other|. Returns true when the row grew.
  bool Merge(const EffectRow& other);

  // Every effect of |other| that this row does not have. What a diagnostic
  // about an undeclared effect needs to name.
  EffectRow Missing(const EffectRow& other) const;

  friend bool operator==(const EffectRow& lhs, const EffectRow& rhs) {
    return lhs.effects_ == rhs.effects_;
  }
  friend bool operator!=(const EffectRow& lhs, const EffectRow& rhs) {
    return !(lhs == rhs);
  }

  // Renders the row as it would be written: "io, alloc". An empty row is "no
  // effects".
  std::string Describe(const StringInterner& interner,
                       const std::function<Symbol(EffectId)>& name_of) const;

 private:
  // Sorted and deduplicated, so equality is a vector compare.
  std::vector<EffectId> effects_;
};

}  // namespace wolang

#endif  // WOLANG_SEMA_EFFECT_H_
