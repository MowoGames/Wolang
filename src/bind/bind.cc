// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/bind/bind.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace wolang {
namespace bind {
namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool IsNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

std::string Trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && IsSpace(text[begin])) {
    ++begin;
  }
  while (end > begin && IsSpace(text[end - 1])) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

// Comments out, line continuations joined, and runs of space collapsed:
// what is left is declarations separated by semicolons and braces.
std::string Flatten(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') {
      i += 2;
      continue;
    }
    if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        ++i;
      }
      i += 2;
      out.push_back(' ');
      continue;
    }
    if (text[i] == '"' || text[i] == '\'') {
      // A string in a header is in a macro or an `asm`; either way it is
      // not part of a declaration, so it travels as one space.
      const char quote = text[i++];
      while (i < text.size() && text[i] != quote) {
        i += text[i] == '\\' ? 2 : 1;
      }
      ++i;
      out.push_back(' ');
      continue;
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

// What a C type means here.
//
// Deliberately narrow: the types a C ABI actually passes in registers,
// plus pointers as addresses. Anything else is reported rather than
// guessed at, because a wrong width in a binding is a bug that shows up
// as nonsense a long way from here.
struct Mapper {
  std::vector<std::pair<std::string, std::string>> aliases;
  // Names of types whose contents a header does not give: `typedef
  // struct window window;`. A pointer to one is an address and nothing
  // more, which is the whole point of declaring it that way.
  std::vector<std::string> opaque;

  bool IsOpaque(std::string_view name) const {
    for (const std::string& held : opaque) {
      if (held == name) {
        return true;
      }
    }
    return false;
  }

  std::string Map(std::string_view spelling, std::string* why) const {
    std::string text = Trim(spelling);
    // `const` says nothing about the ABI; `struct`, `union` and `enum`
    // are noise in front of a name.
    for (const char* noise : {"const ", "volatile ", "struct ", "union ",
                              "enum ", "register ", "restrict "}) {
      for (;;) {
        const size_t at = text.find(noise);
        if (at == std::string::npos) {
          break;
        }
        text.erase(at, std::string_view(noise).size());
      }
    }
    text = Trim(text);

    // A pointer is an address, and the only thing that changes is whether
    // what it points at may be written.
    if (!text.empty() && text.back() == '*') {
      std::string inner = Trim(text.substr(0, text.size() - 1));
      const bool is_const = spelling.find("const") != std::string_view::npos;
      // A pointer to a pointer, or to anything this does not know, is an
      // address all the same.
      if (inner == "char" || inner == "void" || inner.empty() ||
          inner.back() == '*' || IsOpaque(inner)) {
        return is_const ? "*u8" : "*mut u8";
      }
      std::string ignored;
      const std::string pointed = Map(inner, &ignored);
      if (pointed.empty() || pointed == "()") {
        return is_const ? "*u8" : "*mut u8";
      }
      return (is_const ? "*" : "*mut ") + pointed;
    }

    static const std::array<std::pair<const char*, const char*>, 34> kTable{{
        {"void", "()"},
        {"_Bool", "bool"},
        {"bool", "bool"},
        {"char", "s8"},
        {"signed char", "s8"},
        {"unsigned char", "u8"},
        {"short", "s16"},
        {"short int", "s16"},
        {"unsigned short", "u16"},
        {"unsigned short int", "u16"},
        {"int", "s32"},
        {"signed", "s32"},
        {"signed int", "s32"},
        {"unsigned", "u32"},
        {"unsigned int", "u32"},
        {"long", "s64"},
        {"long int", "s64"},
        {"unsigned long", "u64"},
        {"unsigned long int", "u64"},
        {"long long", "s64"},
        {"long long int", "s64"},
        {"unsigned long long", "u64"},
        {"unsigned long long int", "u64"},
        {"float", "f32"},
        {"double", "f64"},
        {"long double", "f64"},
        {"size_t", "u64"},
        {"ssize_t", "s64"},
        {"ptrdiff_t", "s64"},
        {"intptr_t", "s64"},
        {"uintptr_t", "u64"},
        {"int8_t", "s8"},
        {"uint8_t", "u8"},
        {"int16_t", "s16"},
    }};
    for (const auto& [from, to] : kTable) {
      if (text == from) {
        return to;
      }
    }
    // The rest of the sized integers, which follow a pattern.
    static const std::array<std::pair<const char*, const char*>, 6> kSized{{
        {"uint16_t", "u16"},
        {"int32_t", "s32"},
        {"uint32_t", "u32"},
        {"int64_t", "s64"},
        {"uint64_t", "u64"},
        {"wchar_t", "s32"},
    }};
    for (const auto& [from, to] : kSized) {
      if (text == from) {
        return to;
      }
    }
    for (const auto& [name, mapped] : aliases) {
      if (text == name) {
        return mapped;
      }
    }
    if (why != nullptr) {
      *why = text;
    }
    return std::string();
  }
};

// One declaration, as it appears between semicolons.
struct Declaration {
  std::string text;
};

// Splits the flattened text into declarations, keeping enum bodies whole
// and throwing away the bodies of everything else: a function's body is
// not in a header, and a struct's layout is not something this generates.
std::vector<Declaration> Split(const std::string& text) {
  std::vector<Declaration> out;
  std::string current;
  int depth = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '{') {
      ++depth;
      current.push_back(c);
      continue;
    }
    if (c == '}') {
      --depth;
      current.push_back(c);
      if (depth != 0) {
        continue;
      }
      // A body ended, and what follows decides where the declaration
      // does. `typedef struct {...} name;` and `struct x {...} value;`
      // carry a name after the brace; an inline function's body carries
      // the *next* declaration, and keeping it would swallow one (D130).
      size_t at = i + 1;
      while (at < text.size() && text[at] != ';') {
        ++at;
      }
      const std::string tail = Trim(
          std::string_view(text).substr(i + 1, at - i - 1));
      bool only_names = true;
      for (const char held : tail) {
        only_names &= IsNameChar(held) || held == '*' || held == ',' ||
                      IsSpace(held);
      }
      if (only_names && at < text.size()) {
        current.append(" ");
        current.append(tail);
        i = at;
      }
      out.push_back(Declaration{Trim(current)});
      current.clear();
      continue;
    }
    if (c == ';' && depth == 0) {
      out.push_back(Declaration{Trim(current)});
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!Trim(current).empty()) {
    out.push_back(Declaration{Trim(current)});
  }
  return out;
}

// `a, b, c` split at the top level, so that a function pointer's own
// parentheses do not split a parameter in half.
std::vector<std::string> SplitArguments(std::string_view text) {
  std::vector<std::string> out;
  int depth = 0;
  std::string current;
  for (const char c : text) {
    if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    }
    if (c == ',' && depth == 0) {
      out.push_back(Trim(current));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!Trim(current).empty()) {
    out.push_back(Trim(current));
  }
  return out;
}

// The name a parameter or a declarator ends with, and the type in front
// of it. `const char *name` is `const char *` and `name`.
bool SplitDeclarator(std::string_view text,
                     std::string* type,
                     std::string* name) {
  std::string flat = Trim(text);
  if (flat.empty()) {
    return false;
  }
  // An array parameter is a pointer, which is what C says too.
  const size_t bracket = flat.find('[');
  if (bracket != std::string::npos) {
    flat = Trim(flat.substr(0, bracket)) + " *";
  }
  size_t end = flat.size();
  while (end > 0 && IsSpace(flat[end - 1])) {
    --end;
  }
  size_t begin = end;
  while (begin > 0 && IsNameChar(flat[begin - 1])) {
    --begin;
  }
  if (begin == end) {
    // No name: `int` or `const char *`, which is a legal declaration of
    // a parameter and tells us only its type.
    *type = flat;
    name->clear();
    return true;
  }
  *name = flat.substr(begin, end - begin);
  *type = Trim(flat.substr(0, begin));
  if (type->empty()) {
    // One word: it was the type, not the name.
    *type = *name;
    name->clear();
  }
  return true;
}

bool LooksLikeNumber(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  size_t at = 0;
  if (text[at] == '-' || text[at] == '+') {
    ++at;
  }
  bool digits = false;
  for (; at < text.size(); ++at) {
    const char c = text[at];
    if (c >= '0' && c <= '9') {
      digits = true;
      continue;
    }
    if (c == 'x' || c == 'X' || (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F')) {
      continue;
    }
    if (c == 'u' || c == 'U' || c == 'l' || c == 'L') {
      continue;
    }
    return false;
  }
  return digits;
}

}  // namespace

std::string Preprocess(const std::string& path,
                       const std::vector<std::string>& include_paths) {
  std::string command = "cc -E -P";
  for (const std::string& include : include_paths) {
    command.append(" -I'");
    command.append(include);
    command.append("'");
  }
  command.append(" '");
  command.append(path);
  command.append("' 2>/dev/null");
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return std::string();
  }
  std::string out;
  char buffer[4096];
  while (std::size_t read = std::fread(buffer, 1, sizeof(buffer), pipe)) {
    out.append(buffer, read);
  }
  const int status = ::pclose(pipe);
  if (status != 0) {
    return std::string();
  }
  return out;
}

Result Generate(const std::string& text, const Options& options) {
  Result result;
  Mapper mapper;
  const std::string flat = Flatten(text);
  const std::vector<Declaration> declarations = Split(flat);

  std::string functions;
  std::string constants;
  std::string aliases;

  // `--prefix demo,DEMO_` keeps both: a C library names its functions
  // one way and its constants another, and matching is case sensitive
  // because C is.
  std::vector<std::string> prefixes;
  for (size_t begin = 0; begin <= options.prefix.size();) {
    const size_t end = std::min(options.prefix.find(',', begin),
                                options.prefix.size());
    const std::string piece = Trim(
        std::string_view(options.prefix).substr(begin, end - begin));
    if (!piece.empty()) {
      prefixes.push_back(piece);
    }
    begin = end + 1;
  }
  const auto wanted = [&](const std::string& name) {
    if (prefixes.empty()) {
      return true;
    }
    for (const std::string& prefix : prefixes) {
      if (name.rfind(prefix, 0) == 0) {
        return true;
      }
    }
    return false;
  };
  const std::string lead = options.offer ? "offer " : "";

  // Two passes: the aliases first, so that a function using one is
  // readable by the time it is reached.
  for (const Declaration& declaration : declarations) {
    const std::string& line = declaration.text;
    if (line.rfind("typedef", 0) != 0) {
      continue;
    }
    if (line.find('(') != std::string::npos ||
        line.find('{') != std::string::npos) {
      // A function pointer or a struct body: the first is an address and
      // the second is not something this generates.
      std::string type;
      std::string name;
      SplitDeclarator(line, &type, &name);
      if (!name.empty()) {
        // A struct whose contents are not here, or a function pointer:
        // an address either way, and not something to emit a name for.
        mapper.opaque.push_back(name);
        mapper.aliases.emplace_back(name, "*mut u8");
      }
      continue;
    }
    std::string type;
    std::string name;
    if (!SplitDeclarator(line.substr(7), &type, &name) || name.empty()) {
      continue;
    }
    std::string why;
    const std::string mapped = mapper.Map(type, &why);
    if (mapped.empty()) {
      // An alias of something unknown is still a name a later
      // declaration may use: an address is the safe reading.
      mapper.opaque.push_back(name);
      mapper.aliases.emplace_back(name, "*mut u8");
      continue;
    }
    mapper.aliases.emplace_back(name, mapped);
    if (wanted(name) && mapped != "()") {
      aliases.append(lead);
      aliases.append("type ");
      aliases.append(name);
      aliases.append(" = ");
      aliases.append(mapped);
      aliases.append("\n");
      ++result.aliases;
    }
  }

  for (const Declaration& declaration : declarations) {
    const std::string& line = declaration.text;
    if (line.rfind("typedef", 0) == 0) {
      continue;
    }

    // An enum: its members are constants, which is what a C program uses
    // them as.
    const size_t brace = line.find('{');
    if (line.rfind("enum", 0) == 0 && brace != std::string::npos) {
      const size_t close = line.rfind('}');
      if (close == std::string::npos || close < brace) {
        continue;
      }
      int64_t next = 0;
      for (const std::string& member :
           SplitArguments(line.substr(brace + 1, close - brace - 1))) {
        const size_t equals = member.find('=');
        std::string name = Trim(
            equals == std::string::npos ? member : member.substr(0, equals));
        if (name.empty() || !wanted(name)) {
          continue;
        }
        std::string value = std::to_string(next);
        if (equals != std::string::npos) {
          const std::string said = Trim(member.substr(equals + 1));
          if (!LooksLikeNumber(said)) {
            result.skipped.push_back(name + ": its value is not a number");
            continue;
          }
          value = said;
          next = std::strtoll(said.c_str(), nullptr, 0);
        }
        ++next;
        constants.append(lead);
        constants.append("const ");
        constants.append(name);
        constants.append(": int = ");
        constants.append(value);
        constants.append("\n");
        ++result.constants;
      }
      continue;
    }

    // A function: a declarator with parentheses and no body.
    const size_t open = line.find('(');
    if (open == std::string::npos) {
      continue;
    }
    const size_t close = line.rfind(')');
    if (close == std::string::npos || close < open) {
      continue;
    }
    std::string head = Trim(line.substr(0, open));
    for (const char* noise : {"extern", "static", "inline", "__inline",
                              "_Noreturn"}) {
      const size_t at = head.find(noise);
      if (at == 0) {
        head.erase(0, std::string_view(noise).size());
        head = Trim(head);
      }
    }
    std::string returns;
    std::string name;
    if (!SplitDeclarator(head, &returns, &name) || name.empty()) {
      continue;
    }
    if (!wanted(name)) {
      continue;
    }
    if (head.find('(') != std::string::npos) {
      result.skipped.push_back(name + ": returns a function pointer");
      continue;
    }

    const std::string arguments = line.substr(open + 1, close - open - 1);
    if (arguments.find("...") != std::string::npos) {
      result.skipped.push_back(name + ": takes a variable number of "
                                      "arguments");
      continue;
    }

    std::string why;
    std::string result_type = mapper.Map(returns, &why);
    if (result_type.empty()) {
      result.skipped.push_back(name + ": returns `" + why +
                               "`, which has no mapping");
      continue;
    }

    std::string parameters;
    bool ok = true;
    int index = 0;
    for (const std::string& argument : SplitArguments(arguments)) {
      if (argument == "void") {
        continue;
      }
      std::string type;
      std::string parameter;
      SplitDeclarator(argument, &type, &parameter);
      if (argument.find("(*") != std::string::npos) {
        // A callback. An address is what it is at the boundary, and a
        // program that passes one has to make it itself.
        type = "void *";
        if (parameter.empty()) {
          parameter = "callback";
        }
      }
      std::string reason;
      const std::string mapped = mapper.Map(type, &reason);
      if (mapped.empty() || mapped == "()") {
        result.skipped.push_back(name + ": takes `" + reason +
                                 "`, which has no mapping");
        ok = false;
        break;
      }
      if (parameter.empty()) {
        parameter = "arg" + std::to_string(index);
      }
      if (!parameters.empty()) {
        parameters.append(", ");
      }
      parameters.append(parameter);
      parameters.append(": ");
      parameters.append(mapped);
      ++index;
    }
    if (!ok) {
      continue;
    }

    functions.append("extern \"C\" fun ");
    functions.append(name);
    functions.append("(");
    functions.append(parameters);
    functions.append(")");
    if (result_type != "()") {
      functions.append(" -> ");
      functions.append(result_type);
    }
    functions.append("\n");
    ++result.functions;
  }

  std::string out;
  out.append("// Bindings for ");
  out.append(options.header.empty() ? "a C library" : options.header);
  out.append(".\n");
  out.append("//\n");
  out.append("// Generated by `wolang bind`. Everything here is the C ABI:\n");
  out.append("// calling one of these is `unsafe`, a pointer is an address\n");
  out.append("// and nothing checks what is behind it. Edit it -- a\n");
  out.append("// generated binding is a starting point, and the useful\n");
  out.append("// shape is usually a safe wrapper over a few of these.\n");
  if (!aliases.empty()) {
    out.append("\n");
    out.append(aliases);
  }
  if (!constants.empty()) {
    out.append("\n");
    out.append(constants);
  }
  if (!functions.empty()) {
    out.append("\n");
    out.append(functions);
  }
  result.source = out;
  return result;
}

}  // namespace bind
}  // namespace wolang
