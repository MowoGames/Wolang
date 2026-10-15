// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/lsp/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace wolang {
namespace lsp {
namespace {

const Json& Nothing() {
  static const Json nothing;
  return nothing;
}

void WriteString(const std::string& value, std::string* out) {
  out->push_back('"');
  for (const char letter : value) {
    switch (letter) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(letter) < 0x20) {
          char escape[7];
          std::snprintf(escape, sizeof(escape), "\\u%04x",
                        static_cast<unsigned char>(letter));
          out->append(escape);
        } else {
          out->push_back(letter);
        }
        break;
    }
  }
  out->push_back('"');
}

// Where the reader is, so that a malformed message stops rather than
// loops.
struct Reader {
  std::string_view text;
  size_t at = 0;

  void Space() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\n' ||
                                text[at] == '\r' || text[at] == '\t')) {
      ++at;
    }
  }
  bool Eat(char letter) {
    Space();
    if (at < text.size() && text[at] == letter) {
      ++at;
      return true;
    }
    return false;
  }
  char Peek() {
    Space();
    return at < text.size() ? text[at] : '\0';
  }
};

Json ReadValue(Reader* reader);

std::string ReadString(Reader* reader) {
  std::string value;
  if (!reader->Eat('"')) {
    return value;
  }
  while (reader->at < reader->text.size()) {
    const char letter = reader->text[reader->at++];
    if (letter == '"') {
      return value;
    }
    if (letter != '\\') {
      value.push_back(letter);
      continue;
    }
    if (reader->at >= reader->text.size()) {
      return value;
    }
    const char escape = reader->text[reader->at++];
    switch (escape) {
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      case 'b':
        value.push_back('\b');
        break;
      case 'f':
        value.push_back('\f');
        break;
      case 'u': {
        uint32_t code = 0;
        for (int i = 0; i < 4 && reader->at < reader->text.size(); ++i) {
          const char digit = reader->text[reader->at++];
          code *= 16;
          if (digit >= '0' && digit <= '9') {
            code += static_cast<uint32_t>(digit - '0');
          } else if (digit >= 'a' && digit <= 'f') {
            code += static_cast<uint32_t>(digit - 'a' + 10);
          } else if (digit >= 'A' && digit <= 'F') {
            code += static_cast<uint32_t>(digit - 'A' + 10);
          }
        }
        // As UTF-8, which is what every editor sends and expects.
        if (code < 0x80) {
          value.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
          value.push_back(static_cast<char>(0xC0 | (code >> 6)));
          value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          value.push_back(static_cast<char>(0xE0 | (code >> 12)));
          value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        break;
      }
      default:
        value.push_back(escape);
        break;
    }
  }
  return value;
}

Json ReadValue(Reader* reader) {
  const char letter = reader->Peek();
  if (letter == '{') {
    reader->Eat('{');
    Json object = Json::Object();
    if (reader->Eat('}')) {
      return object;
    }
    while (reader->at < reader->text.size()) {
      const std::string name = ReadString(reader);
      if (!reader->Eat(':')) {
        break;
      }
      object.Set(name, ReadValue(reader));
      if (reader->Eat(',')) {
        continue;
      }
      reader->Eat('}');
      break;
    }
    return object;
  }
  if (letter == '[') {
    reader->Eat('[');
    Json array = Json::Array();
    if (reader->Eat(']')) {
      return array;
    }
    while (reader->at < reader->text.size()) {
      array.Push(ReadValue(reader));
      if (reader->Eat(',')) {
        continue;
      }
      reader->Eat(']');
      break;
    }
    return array;
  }
  if (letter == '"') {
    return Json::String(ReadString(reader));
  }
  if (letter == 't') {
    reader->at += 4;
    return Json::Bool(true);
  }
  if (letter == 'f') {
    reader->at += 5;
    return Json::Bool(false);
  }
  if (letter == 'n') {
    reader->at += 4;
    return Json();
  }
  // A number.
  reader->Space();
  const size_t start = reader->at;
  while (reader->at < reader->text.size()) {
    const char digit = reader->text[reader->at];
    const bool part = (digit >= '0' && digit <= '9') || digit == '-' ||
                      digit == '+' || digit == '.' || digit == 'e' ||
                      digit == 'E';
    if (!part) {
      break;
    }
    ++reader->at;
  }
  if (start == reader->at) {
    // Nothing a value can start with: stop rather than spin.
    reader->at = reader->text.size();
    return Json();
  }
  return Json::Number(
      std::strtod(std::string(reader->text.substr(start, reader->at - start))
                      .c_str(),
                  nullptr));
}

}  // namespace

Json Json::Bool(bool value) {
  Json json;
  json.kind_ = Kind::kBool;
  json.boolean_ = value;
  return json;
}

Json Json::Number(double value) {
  Json json;
  json.kind_ = Kind::kNumber;
  json.number_ = value;
  return json;
}

Json Json::String(std::string value) {
  Json json;
  json.kind_ = Kind::kString;
  json.string_ = std::move(value);
  return json;
}

Json Json::Array() {
  Json json;
  json.kind_ = Kind::kArray;
  return json;
}

Json Json::Object() {
  Json json;
  json.kind_ = Kind::kObject;
  return json;
}

bool Json::AsBool(bool fallback) const {
  return kind_ == Kind::kBool ? boolean_ : fallback;
}

double Json::AsNumber(double fallback) const {
  return kind_ == Kind::kNumber ? number_ : fallback;
}

int64_t Json::AsInt(int64_t fallback) const {
  return kind_ == Kind::kNumber ? static_cast<int64_t>(number_) : fallback;
}

const std::string& Json::AsString() const {
  static const std::string empty;
  return kind_ == Kind::kString ? string_ : empty;
}

const Json& Json::operator[](const std::string& name) const {
  for (const auto& [held, value] : object_) {
    if (held == name) {
      return value;
    }
  }
  return Nothing();
}

void Json::Set(const std::string& name, Json value) {
  kind_ = Kind::kObject;
  for (auto& [held, existing] : object_) {
    if (held == name) {
      existing = std::move(value);
      return;
    }
  }
  object_.emplace_back(name, std::move(value));
}

bool Json::Has(const std::string& name) const {
  return !(*this)[name].is_null();
}

void Json::Push(Json value) {
  kind_ = Kind::kArray;
  array_.push_back(std::move(value));
}

size_t Json::size() const {
  return array_.size();
}

const Json& Json::At(size_t index) const {
  return index < array_.size() ? array_[index] : Nothing();
}

std::string Json::Write() const {
  std::string out;
  switch (kind_) {
    case Kind::kNull:
      out.append("null");
      break;
    case Kind::kBool:
      out.append(boolean_ ? "true" : "false");
      break;
    case Kind::kNumber: {
      // Whole numbers without a point: an editor reading a line number
      // wants an integer, and `1.0` is a different thing to some of them.
      if (number_ == std::floor(number_) && std::abs(number_) < 1e15) {
        out.append(std::to_string(static_cast<int64_t>(number_)));
      } else {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", number_);
        out.append(buffer);
      }
      break;
    }
    case Kind::kString:
      WriteString(string_, &out);
      break;
    case Kind::kArray: {
      out.push_back('[');
      for (size_t i = 0; i < array_.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        out.append(array_[i].Write());
      }
      out.push_back(']');
      break;
    }
    case Kind::kObject: {
      out.push_back('{');
      bool first = true;
      for (const auto& [name, value] : object_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        WriteString(name, &out);
        out.push_back(':');
        out.append(value.Write());
      }
      out.push_back('}');
      break;
    }
  }
  return out;
}

Json Json::Parse(std::string_view text) {
  Reader reader{text, 0};
  return ReadValue(&reader);
}

}  // namespace lsp
}  // namespace wolang
