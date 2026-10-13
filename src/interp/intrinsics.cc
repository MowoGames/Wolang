// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/interp/intrinsics.h"

#include <cstdio>

namespace wolang {
namespace interp {
namespace {

struct Entry {
  std::string_view name;
  Intrinsic intrinsic;
};

constexpr Entry kIntrinsics[] = {
    {"Print", Intrinsic::kPrint},
    {"ConcatStr", Intrinsic::kConcatStr},
    {"IntToStr", Intrinsic::kIntToStr},
    {"FloatToStr", Intrinsic::kFloatToStr},
    {"BoolToStr", Intrinsic::kBoolToStr},
};

}  // namespace

Intrinsic LookupIntrinsic(std::string_view name) {
  for (const Entry& entry : kIntrinsics) {
    if (entry.name == name) {
      return entry.intrinsic;
    }
  }
  return Intrinsic::kNone;
}

Value CallIntrinsic(Intrinsic intrinsic,
                    const std::vector<Value>& arguments,
                    Output* output) {
  const auto argument = [&arguments](size_t index) -> const Value& {
    static const Value kMissing;
    return index < arguments.size() ? arguments[index] : kMissing;
  };

  switch (intrinsic) {
    case Intrinsic::kPrint: {
      const Value& text = argument(0);
      if (text.kind() != Value::Kind::kString) {
        return Value();
      }
      if (output != nullptr) {
        output->text.append(text.text());
        output->text.push_back('\n');
      }
      return Value::Void();
    }

    case Intrinsic::kConcatStr: {
      const Value& left = argument(0);
      const Value& right = argument(1);
      if (left.kind() != Value::Kind::kString ||
          right.kind() != Value::Kind::kString) {
        return Value();
      }
      return Value::Str(left.text() + right.text());
    }

    case Intrinsic::kIntToStr: {
      const Value& number = argument(0);
      if (number.kind() != Value::Kind::kInt) {
        return Value();
      }
      return Value::Str(number.is_signed()
                            ? std::to_string(number.signed_integer())
                            : std::to_string(number.integer()));
    }

    case Intrinsic::kFloatToStr: {
      const Value& number = argument(0);
      if (number.kind() != Value::Kind::kFloat) {
        return Value();
      }
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%g", number.floating());
      return Value::Str(buffer);
    }

    case Intrinsic::kBoolToStr: {
      const Value& flag = argument(0);
      if (flag.kind() != Value::Kind::kBool) {
        return Value();
      }
      return Value::Str(flag.boolean() ? "true" : "false");
    }

    case Intrinsic::kNone:
      return Value();
  }
  return Value();
}

}  // namespace interp
}  // namespace wolang
