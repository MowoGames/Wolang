// Copyright 2026 The Wolang Authors. All rights reserved.
//
// The embedding API, exercised the way a host does: load something, call
// it, hand a function back in.

#include "wolang/embed/wolang.h"

#include <cstring>
#include <string>

#include "harness/test.h"

namespace {

// What a script calls back into. The signature is the contract the
// script's `extern "C"` declaration states.
int32_t test_host_double(int32_t value) {
  return value * 2;
}

std::string TextOf(const WolangValue& value) {
  if (value.kind != kWolangText || value.text == nullptr) {
    return std::string();
  }
  return std::string(value.text, static_cast<size_t>(value.length));
}

}  // namespace

WOLANG_TEST(EmbedTest, CallsAFunctionInLoadedSource) {
  WolangVm* vm = wolang_vm_create();
  const int32_t loaded = wolang_vm_load_source(vm, "script.wo", R"(
offer fun Multiply(a: int, b: int) -> int => a * b

offer fun Flag(on: bool) -> bool => !on

offer fun Half(value: float) -> float => value / 2.0

fun Hidden() -> int => 7
)");
  WOLANG_EXPECT_EQ(loaded, 0);
  WOLANG_EXPECT(wolang_vm_ok(vm) == 1);

  // Three offered functions, and the one that was not offered is not
  // part of what the script offers a host either (D128).
  WOLANG_EXPECT_EQ(wolang_vm_function_count(vm), 3);
  WOLANG_EXPECT(wolang_vm_has(vm, "Multiply") == 1);
  WOLANG_EXPECT(wolang_vm_has(vm, "Hidden") == 0);

  WolangValue arguments[2] = {wolang_int(6), wolang_int(7)};
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Multiply", arguments, 2, &answer), 0);
  WOLANG_EXPECT_EQ(answer.kind, kWolangInt);
  WOLANG_EXPECT_EQ(answer.integer, 42);

  arguments[0] = wolang_bool(1);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Flag", arguments, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.kind, kWolangBool);
  WOLANG_EXPECT_EQ(answer.boolean, 0);

  arguments[0] = wolang_float(5.0);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Half", arguments, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.kind, kWolangFloat);
  WOLANG_EXPECT(answer.number > 2.4 && answer.number < 2.6);

  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, PassesTextBothWays) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "text.wo", R"(
offer fun Longer(left: text, right: text) -> text {
  if left.len >= right.len {
    return left
  }
  return right
}

offer fun Size(value: text) -> int => value.len
)"), 0);

  const char* left = "wolang";
  const char* right = "hi";
  WolangValue arguments[2] = {
      wolang_text(left, std::strlen(left)),
      wolang_text(right, std::strlen(right)),
  };
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Longer", arguments, 2, &answer), 0);
  WOLANG_EXPECT_EQ(TextOf(answer), std::string("wolang"));

  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Size", arguments, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 6);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, CallsBackIntoTheHost) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(
      wolang_vm_bind(vm, "test_host_double",
                     reinterpret_cast<void*>(&test_host_double)),
      0);
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "callback.wo", R"(
extern "C" fun test_host_double(value: s32) -> s32

offer fun Quadruple(value: int) -> int uses unsafe {
  return test_host_double(test_host_double(value as s32)) as int
}
)"), 0);

  WolangValue argument = wolang_int(5);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Quadruple", &argument, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 20);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, SaysWhatIsWrongWithAProgram) {
  WolangVm* vm = wolang_vm_create();
  const int32_t loaded =
      wolang_vm_load_source(vm, "broken.wo", "offer fun F() -> int => \"x\"");
  WOLANG_EXPECT(loaded != 0);
  WOLANG_EXPECT(wolang_vm_ok(vm) == 0);
  // The same diagnostic the command line gives, rendered without colour.
  const std::string errors(wolang_vm_errors(vm));
  WOLANG_EXPECT(errors.find("expected `s32`") != std::string::npos);
  WOLANG_EXPECT(errors.find("broken.wo") != std::string::npos);

  // And a call into a program with errors is refused rather than run.
  WOLANG_EXPECT(wolang_vm_call(vm, "F", nullptr, 0, nullptr) != 0);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, RefusesTheWrongNumberOfArguments) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(
      wolang_vm_load_source(vm, "arity.wo", "offer fun Add(a: int, b: int) -> int => a + b"),
      0);
  WolangValue one = wolang_int(1);
  WOLANG_EXPECT(wolang_vm_call(vm, "Add", &one, 1, nullptr) != 0);
  WOLANG_EXPECT(std::string(wolang_vm_errors(vm)).find("takes 2") !=
                     std::string::npos);
  WOLANG_EXPECT(wolang_vm_call(vm, "Missing", nullptr, 0, nullptr) != 0);
  wolang_vm_destroy(vm);
}
