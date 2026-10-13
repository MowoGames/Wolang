// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/dimension.h"

#include <algorithm>
#include <cstdio>

namespace wolang {

Dimensions Dimensions::Base(DimensionId dimension) {
  Dimensions result;
  if (dimension.is_valid()) {
    result.factors_.push_back(Factor{dimension, 1});
  }
  return result;
}

int32_t Dimensions::ExponentOf(DimensionId dimension) const {
  for (const Factor& factor : factors_) {
    if (factor.dimension == dimension) {
      return factor.exponent;
    }
  }
  return 0;
}

void Dimensions::Insert(DimensionId dimension, int32_t exponent) {
  if (exponent == 0 || !dimension.is_valid()) {
    return;
  }
  const auto it = std::lower_bound(
      factors_.begin(), factors_.end(), dimension,
      [](const Factor& factor, DimensionId value) {
        return factor.dimension.index < value.index;
      });
  if (it != factors_.end() && it->dimension == dimension) {
    it->exponent += exponent;
    if (it->exponent == 0) {
      // Dropping cancelled factors is what keeps equality a plain comparison.
      factors_.erase(it);
    }
    return;
  }
  factors_.insert(it, Factor{dimension, exponent});
}

Dimensions Dimensions::Multiply(const Dimensions& other) const {
  Dimensions result = *this;
  for (const Factor& factor : other.factors_) {
    result.Insert(factor.dimension, factor.exponent);
  }
  return result;
}

Dimensions Dimensions::Divide(const Dimensions& other) const {
  Dimensions result = *this;
  for (const Factor& factor : other.factors_) {
    result.Insert(factor.dimension, -factor.exponent);
  }
  return result;
}

Dimensions Dimensions::Power(int32_t exponent) const {
  if (exponent == 0) {
    return Dimensions();
  }
  Dimensions result;
  for (const Factor& factor : factors_) {
    result.Insert(factor.dimension, factor.exponent * exponent);
  }
  return result;
}

bool Dimensions::TryRoot(int32_t root, Dimensions* out) const {
  if (root == 0) {
    return false;
  }
  Dimensions result;
  for (const Factor& factor : factors_) {
    if (factor.exponent % root != 0) {
      // `Sqrt` of a Length is not expressible, which is the right answer.
      return false;
    }
    result.Insert(factor.dimension, factor.exponent / root);
  }
  if (out != nullptr) {
    *out = result;
  }
  return true;
}

std::string Dimensions::Describe(
    const StringInterner& interner,
    const std::function<Symbol(DimensionId)>& name_of) const {
  if (factors_.empty()) {
    return "1";
  }

  const auto text_of = [&](DimensionId dimension) -> std::string {
    const Symbol name = name_of(dimension);
    if (name.is_valid()) {
      return std::string(interner.Text(name));
    }
    return "?";
  };
  const auto append_power = [](std::string* out, int32_t exponent) {
    if (exponent == 1) {
      return;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "^%d", exponent);
    out->append(buffer);
  };

  std::string numerator;
  std::string denominator;
  for (const Factor& factor : factors_) {
    std::string* target = factor.exponent > 0 ? &numerator : &denominator;
    if (!target->empty()) {
      target->push_back('*');
    }
    target->append(text_of(factor.dimension));
    append_power(target, factor.exponent > 0 ? factor.exponent
                                            : -factor.exponent);
  }

  if (numerator.empty()) {
    numerator = "1";
  }
  if (denominator.empty()) {
    return numerator;
  }
  numerator.push_back('/');
  numerator.append(denominator);
  return numerator;
}

}  // namespace wolang
