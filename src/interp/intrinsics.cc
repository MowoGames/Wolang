// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/interp/intrinsics.h"

#include <cstddef>
#include <cstdio>
#include <string_view>

#include "wolang/runtime/text.h"

namespace wolang {
namespace interp {
namespace {

struct Entry {
  std::string_view name;
  Intrinsic intrinsic;
};

// What is left of this file, and why.
//
// `Print` is written in Wolang over `write`, and the conversions an
// interpolated string needs are calls into the runtime that both tiers
// make. So the only thing still here is a `Print` for a file that was
// compiled on its own: a single file does not link the standard library,
// and a language whose `Print` stops working outside a package would be a
// worse language to try things in.
constexpr Entry kIntrinsics[] = {
    {"Print", Intrinsic::kPrint},
    {"wolang_text_from_int", Intrinsic::kTextFromInt},
    {"wolang_text_from_uint", Intrinsic::kTextFromUint},
    {"wolang_fmod", Intrinsic::kFmod},
    {"wolang_text_from_float", Intrinsic::kTextFromFloat},
    {"wolang_text_from_float32", Intrinsic::kTextFromFloat32},
    {"wolang_text_from_bool", Intrinsic::kTextFromBool},
    {"wolang_text_concat", Intrinsic::kTextConcat},
    {"wolang_text_equal", Intrinsic::kTextEqual},
    {"wolang_text_free", Intrinsic::kTextFree},
};

// A pointer the runtime handed back, as a value. The IR reads the pair out
// of it the same way in every tier.
Value Handle(const runtime::Text* text) {
  return Value::Int(reinterpret_cast<uint64_t>(text), IntWidth::k64,
                    /*is_signed=*/false);
}

// The bytes of a string value: a pointer and a length, in every tier.
std::string_view BytesOf(const Value& value) {
  if (value.kind() != Value::Kind::kAggregate ||
      value.elements().size() != 2) {
    return {};
  }
  const Value& data = value.elements()[0];
  const Value& length = value.elements()[1];
  if (data.kind() != Value::Kind::kInt ||
      length.kind() != Value::Kind::kInt) {
    return {};
  }
  const auto* bytes = reinterpret_cast<const char*>(data.integer());
  if (bytes == nullptr) {
    return {};
  }
  return std::string_view(bytes, static_cast<size_t>(length.integer()));
}

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
  switch (intrinsic) {
    case Intrinsic::kPrint: {
      const std::string_view text =
          BytesOf(arguments.empty() ? Value() : arguments.front());
      if (output != nullptr) {
        output->text.append(text);
        output->text.push_back('\n');
      } else {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
      }
      return Value::Void();
    }

    case Intrinsic::kTextFromInt:
      return Handle(runtime::wolang_text_from_int(
          arguments.empty() ? 0 : arguments[0].signed_integer()));

    case Intrinsic::kTextFromUint:
      return Handle(runtime::wolang_text_from_uint(
          arguments.empty() ? 0 : arguments[0].integer()));

    case Intrinsic::kFmod:
      if (arguments.size() < 2) {
        return Value();
      }
      return Value::Float(
          runtime::wolang_fmod(arguments[0].floating(),
                               arguments[1].floating()),
          FloatWidth::k64);
    case Intrinsic::kTextFromFloat32:
      return Handle(runtime::wolang_text_from_float32(
          arguments.empty() ? 0.0 : arguments[0].floating()));

    case Intrinsic::kTextFromFloat:
      return Handle(runtime::wolang_text_from_float(
          arguments.empty() ? 0.0 : arguments[0].floating()));

    case Intrinsic::kTextFromBool:
      return Handle(runtime::wolang_text_from_bool(
          !arguments.empty() && arguments[0].boolean()));

    case Intrinsic::kTextConcat: {
      if (arguments.size() < 4) {
        return Value();
      }
      return Handle(runtime::wolang_text_concat(
          reinterpret_cast<const uint8_t*>(arguments[0].integer()),
          arguments[1].integer(),
          reinterpret_cast<const uint8_t*>(arguments[2].integer()),
          arguments[3].integer()));
    }

    case Intrinsic::kTextEqual: {
      if (arguments.size() < 4) {
        return Value();
      }
      return Value::Bool(runtime::wolang_text_equal(
          reinterpret_cast<const uint8_t*>(arguments[0].integer()),
          arguments[1].integer(),
          reinterpret_cast<const uint8_t*>(arguments[2].integer()),
          arguments[3].integer()));
    }

    case Intrinsic::kTextFree: {
      if (!arguments.empty()) {
        runtime::wolang_text_free(
            reinterpret_cast<const uint8_t*>(arguments[0].integer()));
      }
      return Value::Void();
    }

    case Intrinsic::kNone:
      return Value();
  }
  return Value();
}

}  // namespace interp
}  // namespace wolang
