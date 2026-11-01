// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/sema/effect.h"

#include <algorithm>
#include <cstddef>

namespace wolang {
namespace {

constexpr std::string_view kBuiltinNames[] = {
    "io", "rand", "time", "net", "gpu", "unsafe", "panic", "async",
};

static_assert(std::size(kBuiltinNames) ==
                  static_cast<size_t>(BuiltinEffect::kCount),
              "every built-in effect needs a spelling");

}  // namespace

std::string_view BuiltinEffectName(BuiltinEffect effect) {
  const auto index = static_cast<size_t>(effect);
  return index < std::size(kBuiltinNames) ? kBuiltinNames[index]
                                          : std::string_view();
}

BuiltinEffect LookupBuiltinEffect(std::string_view text) {
  for (size_t i = 0; i < std::size(kBuiltinNames); ++i) {
    if (kBuiltinNames[i] == text) {
      return static_cast<BuiltinEffect>(i);
    }
  }
  return BuiltinEffect::kCount;
}

EffectRow EffectRow::Single(EffectId effect) {
  EffectRow row;
  row.Add(effect);
  return row;
}

bool EffectRow::Contains(EffectId effect) const {
  return std::binary_search(effects_.begin(), effects_.end(), effect,
                            [](EffectId lhs, EffectId rhs) {
                              return lhs.index < rhs.index;
                            });
}

bool EffectRow::Add(EffectId effect) {
  if (!effect.is_valid()) {
    return false;
  }
  const auto position =
      std::lower_bound(effects_.begin(), effects_.end(), effect,
                       [](EffectId lhs, EffectId rhs) {
                         return lhs.index < rhs.index;
                       });
  if (position != effects_.end() && *position == effect) {
    return false;
  }
  effects_.insert(position, effect);
  return true;
}

bool EffectRow::Merge(const EffectRow& other) {
  bool grew = false;
  for (EffectId effect : other.effects_) {
    grew |= Add(effect);
  }
  return grew;
}

EffectRow EffectRow::Missing(const EffectRow& other) const {
  EffectRow result;
  for (EffectId effect : other.effects_) {
    if (!Contains(effect)) {
      result.Add(effect);
    }
  }
  return result;
}

std::string EffectRow::Describe(
    const StringInterner& interner,
    const std::function<Symbol(EffectId)>& name_of) const {
  if (effects_.empty()) {
    return "no effects";
  }
  std::string result;
  for (EffectId effect : effects_) {
    if (!result.empty()) {
      result.append(", ");
    }
    const Symbol name = name_of(effect);
    result.append(name.is_valid() ? interner.Text(name) : "?");
  }
  return result;
}

}  // namespace wolang
