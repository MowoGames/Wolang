// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/base/string_interner.h"

namespace wolang {

StringInterner::StringInterner() {
  texts_.reserve(1024);
  index_by_text_.reserve(1024);
}

Symbol StringInterner::Intern(std::string_view text) {
  auto it = index_by_text_.find(text);
  if (it != index_by_text_.end()) {
    return Symbol{it->second};
  }

  // The map keys point into the arena, not into the caller's buffer, so the
  // interner stays valid after the source text goes away.
  const std::string_view owned = arena_.CopyString(text);
  const auto index = static_cast<uint32_t>(texts_.size());
  texts_.push_back(owned);
  index_by_text_.emplace(owned, index);
  return Symbol{index};
}

Symbol StringInterner::Lookup(std::string_view text) const {
  auto it = index_by_text_.find(text);
  if (it == index_by_text_.end()) {
    return Symbol{};
  }
  return Symbol{it->second};
}

std::string_view StringInterner::Text(Symbol symbol) const {
  if (!symbol.is_valid() || symbol.index >= texts_.size()) {
    return {};
  }
  return texts_[symbol.index];
}

}  // namespace wolang
