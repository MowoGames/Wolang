// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_BASE_STRING_INTERNER_H_
#define WOLANG_BASE_STRING_INTERNER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wolang/base/arena.h"

namespace wolang {

// A handle for an interned string. Comparing two symbols is one integer
// comparison, which is what lets name lookup, scope maps and IR keys stay
// cheap. Symbols are stable for the lifetime of their interner.
struct Symbol {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }

  friend bool operator==(Symbol lhs, Symbol rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(Symbol lhs, Symbol rhs) { return !(lhs == rhs); }
};

// Owns the text of every identifier, keyword spelling and string literal seen
// by the compiler.
class StringInterner {
 public:
  StringInterner();

  StringInterner(const StringInterner&) = delete;
  StringInterner& operator=(const StringInterner&) = delete;

  // Returns the symbol for |text|, adding it on first use.
  Symbol Intern(std::string_view text);

  // Returns the symbol for |text| if it has been interned, or an invalid
  // symbol otherwise. Never mutates the interner.
  Symbol Lookup(std::string_view text) const;

  // Returns the text behind |symbol|, or an empty view for invalid symbols.
  std::string_view Text(Symbol symbol) const;

  size_t size() const { return texts_.size(); }

 private:
  Arena arena_;
  std::vector<std::string_view> texts_;
  std::unordered_map<std::string_view, uint32_t> index_by_text_;
};

}  // namespace wolang

template <>
struct std::hash<wolang::Symbol> {
  size_t operator()(wolang::Symbol symbol) const noexcept {
    return std::hash<uint32_t>()(symbol.index);
  }
};

#endif  // WOLANG_BASE_STRING_INTERNER_H_
