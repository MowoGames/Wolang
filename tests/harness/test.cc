// Copyright 2026 The Wolang Authors. All rights reserved.

#include "harness/test.h"

#include <cstdio>
#include <string>
#include <utility>

namespace wolang::test {
namespace {

std::vector<TestCase>& MutableTests() {
  static auto* tests = new std::vector<TestCase>();
  return *tests;
}

int g_current_failures = 0;

std::string FullName(const TestCase& test_case) {
  std::string name(test_case.suite);
  name.push_back('.');
  name.append(test_case.name);
  return name;
}

}  // namespace

void Register(TestCase test_case) {
  MutableTests().push_back(test_case);
}

const std::vector<TestCase>& RegisteredTests() {
  return MutableTests();
}

void ReportFailure(std::string_view file, int line, std::string message) {
  ++g_current_failures;
  std::fprintf(stderr, "  %.*s:%d: %s\n", static_cast<int>(file.size()),
               file.data(), line, message.c_str());
}

int RunAll(std::string_view filter, bool verbose) {
  const std::vector<TestCase>& tests = MutableTests();
  int selected = 0;
  int failed = 0;

  for (const TestCase& test_case : tests) {
    const std::string name = FullName(test_case);
    if (!filter.empty() && name.find(filter) == std::string::npos) {
      continue;
    }
    ++selected;

    if (verbose) {
      std::fprintf(stdout, "[ run      ] %s\n", name.c_str());
      std::fflush(stdout);
    }

    g_current_failures = 0;
    test_case.body();
    if (g_current_failures != 0) {
      ++failed;
      std::fprintf(stdout, "[   failed ] %s (%d failure%s)\n", name.c_str(),
                   g_current_failures, g_current_failures == 1 ? "" : "s");
    } else if (verbose) {
      std::fprintf(stdout, "[       ok ] %s\n", name.c_str());
    }
    std::fflush(stdout);
  }

  std::fprintf(stdout, "\n%d test%s run, %d failed\n", selected,
               selected == 1 ? "" : "s", failed);
  if (selected == 0) {
    std::fprintf(stderr, "no tests matched the filter\n");
    return 1;
  }
  return failed;
}

}  // namespace wolang::test
