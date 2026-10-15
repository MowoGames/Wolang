// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// A minimal test harness. The project has no third party dependencies, and
// exceptions are disabled, so assertions report and return instead of
// throwing: WOLANG_ASSERT_* leaves the test body, WOLANG_EXPECT_* continues.
//
//   WOLANG_TEST(Arena, AllocatesAligned) {
//     Arena arena;
//     WOLANG_ASSERT_NE(arena.Allocate(8, 8), nullptr);
//     WOLANG_EXPECT_EQ(arena.bytes_used(), 8u);
//   }

#ifndef WOLANG_TESTS_HARNESS_TEST_H_
#define WOLANG_TESTS_HARNESS_TEST_H_

#include <concepts>
#include <cstddef>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace wolang::test {

using TestBody = void (*)();

struct TestCase {
  std::string_view suite;
  std::string_view name;
  TestBody body;
  std::string_view file;
  int line;
};

// Registers |test_case|; used by the WOLANG_TEST macro.
void Register(TestCase test_case);

const std::vector<TestCase>& RegisteredTests();

// Records a failure against the running test and prints it.
void ReportFailure(std::string_view file, int line, std::string message);

// Runs every registered test whose "suite.name" contains |filter|. Returns the
// number of failed tests.
int RunAll(std::string_view filter, bool verbose);

struct Registrar {
  Registrar(std::string_view suite,
            std::string_view name,
            TestBody body,
            std::string_view file,
            int line) {
    Register(TestCase{suite, name, body, file, line});
  }
};

template <typename T>
concept Streamable = requires(std::ostream& stream, const T& value) {
  stream << value;
};

template <typename T>
std::string Stringify(const T& value) {
  if constexpr (std::is_same_v<std::remove_cvref_t<T>, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<std::remove_cvref_t<T>>) {
    std::ostringstream stream;
    stream << static_cast<long long>(value);
    return stream.str();
  } else if constexpr (Streamable<T>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<unprintable>";
  }
}

template <typename A, typename B>
std::string DescribeComparison(std::string_view lhs_text,
                               std::string_view rhs_text,
                               const A& lhs,
                               const B& rhs,
                               std::string_view relation) {
  std::string message(lhs_text);
  message.append(" ");
  message.append(relation);
  message.append(" ");
  message.append(rhs_text);
  message.append("\n    left:  ");
  message.append(Stringify(lhs));
  message.append("\n    right: ");
  message.append(Stringify(rhs));
  return message;
}

}  // namespace wolang::test

#define WOLANG_TEST(suite, name)                                            \
  static void WolangTestBody_##suite##_##name();                            \
  static const ::wolang::test::Registrar WolangTestRegistrar_##suite##_##name(\
      #suite, #name, &WolangTestBody_##suite##_##name, __FILE__, __LINE__); \
  static void WolangTestBody_##suite##_##name()

#define WOLANG_INTERNAL_FAIL(message) \
  ::wolang::test::ReportFailure(__FILE__, __LINE__, (message))

#define WOLANG_INTERNAL_CHECK(cond, on_failure)                 \
  do {                                                          \
    if (!(cond)) {                                              \
      WOLANG_INTERNAL_FAIL(std::string("expected: ") + #cond);  \
      on_failure;                                               \
    }                                                           \
  } while (false)

#define WOLANG_INTERNAL_COMPARE(lhs, rhs, op, relation, on_failure)         \
  do {                                                                      \
    const auto& wolang_lhs = (lhs);                                         \
    const auto& wolang_rhs = (rhs);                                         \
    if (!(wolang_lhs op wolang_rhs)) {                                      \
      WOLANG_INTERNAL_FAIL(::wolang::test::DescribeComparison(              \
          #lhs, #rhs, wolang_lhs, wolang_rhs, relation));                   \
      on_failure;                                                           \
    }                                                                       \
  } while (false)

#define WOLANG_EXPECT(cond) WOLANG_INTERNAL_CHECK(cond, (void)0)
#define WOLANG_ASSERT(cond) WOLANG_INTERNAL_CHECK(cond, return )

#define WOLANG_EXPECT_EQ(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, ==, "==", (void)0)
#define WOLANG_ASSERT_EQ(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, ==, "==", return )

#define WOLANG_EXPECT_NE(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, !=, "!=", (void)0)
#define WOLANG_ASSERT_NE(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, !=, "!=", return )

#define WOLANG_EXPECT_LT(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, <, "<", (void)0)
#define WOLANG_EXPECT_LE(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, <=, "<=", (void)0)
#define WOLANG_EXPECT_GT(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, >, ">", (void)0)
#define WOLANG_EXPECT_GE(lhs, rhs) \
  WOLANG_INTERNAL_COMPARE(lhs, rhs, >=, ">=", (void)0)

#define WOLANG_FAIL(message) WOLANG_INTERNAL_FAIL(std::string(message))

#endif  // WOLANG_TESTS_HARNESS_TEST_H_
