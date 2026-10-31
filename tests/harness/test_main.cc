// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "harness/test.h"

#if defined(_WIN32)
#include <crtdbg.h>
#include <stdlib.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// A failed check in the debug runtime, or a crash, says so on stderr and
// ends the process. Left to itself Windows opens a dialog and waits for a
// click, which on a build machine is a run that never finishes.
void ReportFailuresInsteadOfAsking() {
#if defined(_WIN32)
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}


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
  ReportFailuresInsteadOfAsking();
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
