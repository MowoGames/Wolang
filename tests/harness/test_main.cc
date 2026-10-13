// Copyright 2026 The Wolang Authors. All rights reserved.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "harness/test.h"

namespace {

void PrintUsage() {
  std::fprintf(stdout,
               "usage: wolang_tests [options]\n"
               "\n"
               "  --filter <text>  run tests whose suite.name contains <text>\n"
               "  --list           print the registered tests and exit\n"
               "  -v, --verbose    print every test as it runs\n"
               "  -h, --help       show this message\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string filter;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = std::string(argument.substr(std::strlen("--filter=")));
    } else if (argument == "-v" || argument == "--verbose") {
      verbose = true;
    } else if (argument == "--list") {
      for (const auto& test_case : wolang::test::RegisteredTests()) {
        std::fprintf(stdout, "%.*s.%.*s\n",
                     static_cast<int>(test_case.suite.size()),
                     test_case.suite.data(),
                     static_cast<int>(test_case.name.size()),
                     test_case.name.data());
      }
      return 0;
    } else if (argument == "-h" || argument == "--help") {
      PrintUsage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %.*s\n",
                   static_cast<int>(argument.size()), argument.data());
      PrintUsage();
      return 2;
    }
  }

  return wolang::test::RunAll(filter, verbose) == 0 ? 0 : 1;
}
