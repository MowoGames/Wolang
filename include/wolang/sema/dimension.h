// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SEMA_DIMENSION_H_
#define WOLANG_SEMA_DIMENSION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "wolang/base/string_interner.h"

namespace wolang {

// Identifies a `dimension` declaration.
struct DimensionId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
  friend bool operator==(DimensionId lhs, DimensionId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(DimensionId lhs, DimensionId rhs) {
    return !(lhs == rhs);
  }
};

// A product of base dimensions with integer exponents: `Length/Time` is
// {Length: 1, Time: -1}, and a plain number is the empty product.
//
// Multiplying two quantities adds their exponents, dividing subtracts them,
// and the result is dimensionless exactly when everything cancels. That is
// the whole of dimensional analysis, and it is why `10m / 2s` has a type
// nobody had to declare.
class Dimensions {
 public:
  struct Factor {
    DimensionId dimension;
    int32_t exponent = 0;

    friend bool operator==(const Factor& lhs, const Factor& rhs) {
      return lhs.dimension == rhs.dimension && lhs.exponent == rhs.exponent;
    }
  };

  Dimensions() = default;

  // A single base dimension raised to the first power.
  static Dimensions Base(DimensionId dimension);

  bool is_dimensionless() const { return factors_.empty(); }
  const std::vector<Factor>& factors() const { return factors_; }

  // Returns the exponent of |dimension|, or zero.
  int32_t ExponentOf(DimensionId dimension) const;

  Dimensions Multiply(const Dimensions& other) const;
  Dimensions Divide(const Dimensions& other) const;
  Dimensions Power(int32_t exponent) const;
  // The exponents must all be even, or the result is not representable.
  bool TryRoot(int32_t root, Dimensions* out) const;

  friend bool operator==(const Dimensions& lhs, const Dimensions& rhs) {
    return lhs.factors_ == rhs.factors_;
  }
  friend bool operator!=(const Dimensions& lhs, const Dimensions& rhs) {
    return !(lhs == rhs);
  }

  // Renders the product for diagnostics, for example "Length/Time" or
  // "Length^2". Dimensionless prints as "1".
  std::string Describe(
      const StringInterner& interner,
      const std::vector<Symbol>& dimension_names) const;

 private:
  void Insert(DimensionId dimension, int32_t exponent);

  // Sorted by dimension index with no zero exponents, so two equal products
  // compare equal.
  std::vector<Factor> factors_;
};

}  // namespace wolang

#endif  // WOLANG_SEMA_DIMENSION_H_
