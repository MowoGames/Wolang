// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_TESTS_HARNESS_TEMP_TREE_H_
#define WOLANG_TESTS_HARNESS_TEMP_TREE_H_

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace wolang {
namespace testing {

namespace fs = std::filesystem;

inline int ProcessId() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

// A directory tree created for one test and removed with it. The process id is
// part of the name so that suites running in parallel cannot collide.
class TempTree {
 public:
  explicit TempTree(std::string_view name) {
    std::error_code error;
    root_ = fs::temp_directory_path(error) /
            ("wolang-test-" + std::to_string(ProcessId()) + "-" +
             std::string(name));
    fs::remove_all(root_, error);
    fs::create_directories(root_, error);
  }

  ~TempTree() {
    std::error_code error;
    fs::remove_all(root_, error);
  }

  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  void Write(std::string_view relative, std::string_view text) {
    const fs::path path = root_ / relative;
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  std::string path() const { return root_.string(); }
  std::string path(std::string_view relative) const {
    return (root_ / relative).string();
  }

 private:
  fs::path root_;
};

}  // namespace testing
}  // namespace wolang

#endif  // WOLANG_TESTS_HARNESS_TEMP_TREE_H_
