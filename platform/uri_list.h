// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The `text/uri-list` a file drop arrives as on X11 (XDND), turned into
// paths (D247). Header-only and system-free so it is tested from a
// string, the same as the evdev decoding.

#ifndef WOLANG_PLATFORM_URI_LIST_H_
#define WOLANG_PLATFORM_URI_LIST_H_

#include <string>
#include <vector>

namespace wolang {
namespace platform {

inline int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// One line per URI, CRLF separated, `#` lines are comments. Only
// `file:` URIs on this machine become paths; anything else is dropped
// because a program cannot open it.
inline std::vector<std::string> PathsFromUriList(const std::string& list) {
  std::vector<std::string> paths;
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find('\n', start);
    if (end == std::string::npos) {
      end = list.size();
    }
    std::string line = list.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#' || line.compare(0, 5, "file:") != 0) {
      continue;
    }
    std::string rest = line.substr(5);
    // `file:///path` and `file://localhost/path` name this machine;
    // `file://other/path` does not.
    if (rest.compare(0, 2, "//") == 0) {
      rest = rest.substr(2);
      if (rest.compare(0, 9, "localhost") == 0) {
        rest = rest.substr(9);
      }
      if (rest.empty() || rest[0] != '/') {
        continue;
      }
    }
    std::string path;
    for (size_t i = 0; i < rest.size(); ++i) {
      if (rest[i] == '%' && i + 2 < rest.size() &&
          HexValue(rest[i + 1]) >= 0 && HexValue(rest[i + 2]) >= 0) {
        path.push_back(static_cast<char>(HexValue(rest[i + 1]) * 16 +
                                         HexValue(rest[i + 2])));
        i += 2;
      } else {
        path.push_back(rest[i]);
      }
    }
    if (!path.empty()) {
      paths.push_back(path);
    }
  }
  return paths;
}

}  // namespace platform
}  // namespace wolang

#endif  // WOLANG_PLATFORM_URI_LIST_H_
