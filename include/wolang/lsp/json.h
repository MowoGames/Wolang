// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_LSP_JSON_H_
#define WOLANG_LSP_JSON_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wolang {
namespace lsp {

// JSON, for talking to an editor.
//
// Its own small reader rather than the one in `std.json`: that one is
// Wolang, and this is the compiler talking a protocol over a pipe before
// any Wolang has run (D117). Small on purpose -- what the Language Server
// Protocol sends is objects, arrays, strings, numbers and booleans, and
// nothing here needs to be fast.
class Json {
 public:
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };

  Json() = default;
  static Json Bool(bool value);
  static Json Number(double value);
  static Json String(std::string value);
  static Json Array();
  static Json Object();

  Kind kind() const { return kind_; }
  bool is_null() const { return kind_ == Kind::kNull; }

  bool AsBool(bool fallback = false) const;
  double AsNumber(double fallback = 0) const;
  int64_t AsInt(int64_t fallback = 0) const;
  const std::string& AsString() const;

  // Objects.
  const Json& operator[](const std::string& name) const;
  void Set(const std::string& name, Json value);
  bool Has(const std::string& name) const;

  // Arrays.
  void Push(Json value);
  size_t size() const;
  const Json& At(size_t index) const;

  std::string Write() const;

  // Reads one value. Answers a null value when the text is not JSON,
  // which is what a server that must not die on a bad message needs.
  static Json Parse(std::string_view text);

 private:
  Kind kind_ = Kind::kNull;
  bool boolean_ = false;
  double number_ = 0;
  std::string string_;
  std::vector<Json> array_;
  std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace lsp
}  // namespace wolang

#endif  // WOLANG_LSP_JSON_H_
