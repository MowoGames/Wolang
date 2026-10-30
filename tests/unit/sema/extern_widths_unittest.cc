// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "harness/test.h"

namespace wolang {
namespace {

namespace fs = std::filesystem;

bool IsWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// True when `text` holds the word `int` on its own (not `u32`, `print`).
bool HasBareInt(std::string_view text) {
  size_t at = 0;
  while ((at = text.find("int", at)) != std::string_view::npos) {
    const size_t end = at + 3;
    const bool left = at == 0 || !IsWordChar(text[at - 1]);
    const bool right = end >= text.size() || !IsWordChar(text[end]);
    if (left && right) return true;
    at = end;
  }
  return false;
}

// Scans every `extern "C" fun` in `source`: from the keyword to the end of
// the line that closes the parameter list, so the result type is included.
// Returns how many signatures name a bare `int`; `total` counts them all.
int CountBareIntSignatures(const std::string& source, int* total) {
  static constexpr std::string_view kMarker = "extern \"C\" fun";
  int bad = 0;
  size_t at = 0;
  while ((at = source.find(kMarker, at)) != std::string::npos) {
    size_t i = at + kMarker.size();
    int depth = 0;
    bool closed = false;
    while (i < source.size()) {
      const char c = source[i];
      if (c == '(') ++depth;
      if (c == ')' && --depth == 0) closed = true;
      if (c == '\n' && closed) break;
      ++i;
    }
    ++*total;
    if (HasBareInt(std::string_view(source).substr(at, i - at))) ++bad;
    at = i;
  }
  return bad;
}

// D238: `int` becomes s64 but C's `int` stays 32 bits, so a bare `int` in
// an extern signature would silently widen. Every extern names its width.
WOLANG_TEST(ExternWidths, NoLibraryExternUsesBareInt) {
  int total = 0;
  int bad = 0;
  for (const char* dir : {"lib", "examples"}) {
    const fs::path root = fs::path(WOLANG_PROJECT_DIR) / dir;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec);
    for (; !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (it->path().extension() != ".wo") continue;
      std::ifstream in(it->path());
      std::stringstream text;
      text << in.rdbuf();
      const int found = CountBareIntSignatures(text.str(), &total);
      if (found != 0) WOLANG_FAIL("bare int in extern: " + it->path().string());
      bad += found;
    }
  }
  WOLANG_EXPECT(total > 100);
  WOLANG_EXPECT_EQ(bad, 0);
}

// The twin: the scanner flags a bare `int` (one-line and multi-line) and
// ignores widths and non-extern functions.
WOLANG_TEST(ExternWidths, ScannerFlagsBareIntOnly) {
  int total = 0;
  WOLANG_EXPECT_EQ(
      CountBareIntSignatures("extern \"C\" fun f(a: int) -> s32\n", &total),
      1);
  WOLANG_EXPECT_EQ(
      CountBareIntSignatures(
          "extern \"C\" fun g(\n  a: s32,\n  b: *u8,\n) -> int\n", &total),
      1);
  WOLANG_EXPECT_EQ(
      CountBareIntSignatures(
          "extern \"C\" fun h(a: s32, p: *u8) -> u64\n"
          "fun k(x: int) -> int\n",
          &total),
      0);
  WOLANG_EXPECT_EQ(total, 3);
}

}  // namespace
}  // namespace wolang
