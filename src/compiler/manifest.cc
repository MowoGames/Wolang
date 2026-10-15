// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/manifest.h"

#include <utility>

namespace wolang {
namespace {

constexpr std::string_view kSyntaxError = "E0050";
constexpr std::string_view kUnknownKey = "E0051";
constexpr std::string_view kMissingKey = "E0052";
constexpr std::string_view kInvalidValue = "E0053";
constexpr std::string_view kDuplicateKey = "E0054";

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

std::string_view Trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && IsSpace(text[begin])) {
    ++begin;
  }
  while (end > begin && IsSpace(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// One `key = value` pair, with the span of the value for diagnostics.
struct Field {
  std::string_view key;
  std::string_view value;
  uint32_t key_offset = 0;
  uint32_t value_offset = 0;
};

class ManifestParser {
 public:
  ManifestParser(const SourceManager& sources,
                 FileId file,
                 DiagnosticEngine* diagnostics)
      : file_(file),
        diagnostics_(diagnostics),
        text_(sources.Text(file)) {}

  Manifest Run();

 private:
  SourceSpan Span(uint32_t offset, size_t length) const {
    return SourceSpan{file_, offset, offset + static_cast<uint32_t>(length)};
  }

  void Report(std::string_view code,
              std::string message,
              SourceSpan span,
              std::string label = {}) {
    diagnostics_->Error(code, std::move(message))
        .Primary(span, std::move(label));
  }

  // Strips a trailing comment, honouring quotes so a `#` inside a string
  // survives.
  std::string_view StripComment(std::string_view line) const {
    bool in_string = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"') {
        in_string = !in_string;
      } else if (line[i] == '#' && !in_string) {
        return line.substr(0, i);
      }
    }
    return line;
  }

  // Unquotes a string value, or reports and returns an empty view.
  bool ReadString(const Field& field, std::string* out);
  bool SplitField(std::string_view line, uint32_t offset, Field* out);
  void ApplyPackageField(const Field& field, Manifest* manifest);
  void ApplyDependencyField(const Field& field, Manifest* manifest);
  void ApplyLintField(const Field& field, Manifest* manifest);
  void ApplyBuildField(const Field& field, Manifest* manifest);
  void ApplyLinkField(const Field& field, Manifest* manifest);
  // A comma separated list in a quoted string, the way `capabilities` is
  // spelled. Lists in this subset of TOML are strings, because a parser
  // with arrays is a bigger parser for no more expressiveness here.
  bool ReadList(const Field& field, std::vector<std::string>* out);

  FileId file_;
  DiagnosticEngine* diagnostics_;
  std::string_view text_;
  // Keys already seen in the current table, to catch duplicates.
  std::vector<std::string> seen_;
};

bool ManifestParser::SplitField(std::string_view line,
                                uint32_t offset,
                                Field* out) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) {
    Report(kSyntaxError, "expected `key = value`", Span(offset, line.size()));
    return false;
  }

  const std::string_view raw_key = line.substr(0, equals);
  const std::string_view raw_value = line.substr(equals + 1);
  const std::string_view key = Trim(raw_key);
  const std::string_view value = Trim(raw_value);

  if (key.empty()) {
    Report(kSyntaxError, "expected a key before `=`", Span(offset, 1));
    return false;
  }
  if (value.empty()) {
    Report(kSyntaxError, "expected a value after `=`",
           Span(offset + static_cast<uint32_t>(equals), 1));
    return false;
  }

  out->key = key;
  out->value = value;
  out->key_offset =
      offset + static_cast<uint32_t>(key.data() - line.data());
  out->value_offset =
      offset + static_cast<uint32_t>(value.data() - line.data());
  return true;
}

bool ManifestParser::ReadString(const Field& field, std::string* out) {
  const std::string_view value = field.value;
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    std::string message = "expected a quoted string for `";
    message.append(field.key);
    message.append("`");
    Report(kInvalidValue, std::move(message),
           Span(field.value_offset, value.size()));
    return false;
  }

  out->clear();
  const std::string_view body = value.substr(1, value.size() - 2);
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '\\' && i + 1 < body.size()) {
      ++i;
    }
    out->push_back(body[i]);
  }
  return true;
}

void ManifestParser::ApplyPackageField(const Field& field,
                                       Manifest* manifest) {
  if (field.key == "name") {
    ReadString(field, &manifest->name);
    return;
  }
  if (field.key == "version") {
    ReadString(field, &manifest->version);
    return;
  }
  if (field.key == "source") {
    ReadString(field, &manifest->source_directory);
    return;
  }
  std::string message = "unknown key `";
  message.append(field.key);
  message.append("` in `[package]`");
  Report(kUnknownKey, std::move(message),
         Span(field.key_offset, field.key.size()),
         "known keys are name, version and source");
}

void ManifestParser::ApplyDependencyField(const Field& field,
                                          Manifest* manifest) {
  ManifestDependency dependency;
  dependency.name = std::string(field.key);

  if (field.value.front() == '{') {
    // An inline table: `{ path = "..." }`.
    if (field.value.back() != '}') {
      Report(kSyntaxError, "expected `}` to close the inline table",
             Span(field.value_offset, field.value.size()));
      return;
    }
    const std::string_view body =
        field.value.substr(1, field.value.size() - 2);
    size_t position = 0;
    while (position < body.size()) {
      const size_t comma = body.find(',', position);
      const std::string_view part = Trim(body.substr(
          position, comma == std::string_view::npos ? std::string_view::npos
                                                    : comma - position));
      if (!part.empty()) {
        Field inner;
        const auto inner_offset =
            field.value_offset +
            static_cast<uint32_t>(part.data() - field.value.data());
        if (SplitField(part, inner_offset, &inner)) {
          if (inner.key == "path") {
            ReadString(inner, &dependency.path);
          } else {
            std::string message = "unknown dependency key `";
            message.append(inner.key);
            message.append("`");
            Report(kUnknownKey, std::move(message),
                   Span(inner.key_offset, inner.key.size()),
                   "only `path` dependencies are supported for now");
          }
        }
      }
      if (comma == std::string_view::npos) {
        break;
      }
      position = comma + 1;
    }
  } else if (!ReadString(field, &dependency.path)) {
    return;
  }

  if (dependency.path.empty()) {
    std::string message = "dependency `";
    message.append(field.key);
    message.append("` has no path");
    Report(kMissingKey, std::move(message),
           Span(field.value_offset, field.value.size()),
           "write `name = { path = \"...\" }`");
    return;
  }
  manifest->dependencies.push_back(std::move(dependency));
}

void ManifestParser::ApplyLintField(const Field& field, Manifest* manifest) {
  std::string name;
  if (!ReadString(field, &name)) {
    return;
  }
  Severity severity = Severity::kWarning;
  if (!ParseSeverity(name, &severity)) {
    Report(kInvalidValue, "expected off, hint, perf, warning or error",
           Span(field.value_offset, field.value.size()));
    return;
  }
  manifest->lints.push_back(LintOverride{std::string(field.key), severity});
}

bool ManifestParser::ReadList(const Field& field,
                              std::vector<std::string>* out) {
  std::string joined;
  if (!ReadString(field, &joined)) {
    return false;
  }
  size_t begin = 0;
  while (begin <= joined.size()) {
    size_t end = joined.find(',', begin);
    if (end == std::string::npos) {
      end = joined.size();
    }
    std::string_view piece(joined.data() + begin, end - begin);
    while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) {
      piece.remove_prefix(1);
    }
    while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) {
      piece.remove_suffix(1);
    }
    if (!piece.empty()) {
      out->emplace_back(piece);
    }
    begin = end + 1;
  }
  return true;
}

void ManifestParser::ApplyLinkField(const Field& field, Manifest* manifest) {
  if (field.key == "libraries") {
    ReadList(field, &manifest->link_libraries);
    return;
  }
  if (field.key == "paths") {
    ReadList(field, &manifest->link_paths);
    return;
  }
  if (field.key == "frameworks") {
    ReadList(field, &manifest->link_frameworks);
    return;
  }
  std::string message = "unknown key `";
  message.append(field.key);
  message.append("` in `[link]`");
  Report(kUnknownKey, std::move(message),
         Span(field.key_offset, field.key.size()),
         "known keys are libraries, paths and frameworks");
}

void ManifestParser::ApplyBuildField(const Field& field, Manifest* manifest) {
  if (field.key == "unsafe") {
    std::string policy;
    if (!ReadString(field, &policy)) {
      return;
    }
    if (policy == "allow") {
      manifest->allow_unsafe = true;
    } else if (policy == "deny") {
      manifest->allow_unsafe = false;
    } else {
      Report(kInvalidValue, "expected `allow` or `deny`",
             Span(field.value_offset, field.value.size()));
    }
    return;
  }
  if (field.key == "capabilities") {
    std::string granted;
    if (!ReadString(field, &granted)) {
      return;
    }
    manifest->has_capabilities = true;
    // A comma separated list, spelled the way a `uses` clause is.
    size_t begin = 0;
    while (begin <= granted.size()) {
      size_t end = granted.find(',', begin);
      if (end == std::string::npos) {
        end = granted.size();
      }
      std::string_view name(granted.data() + begin, end - begin);
      while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
        name.remove_prefix(1);
      }
      while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.remove_suffix(1);
      }
      if (!name.empty()) {
        manifest->capabilities.emplace_back(name);
      }
      begin = end + 1;
    }
    return;
  }

  std::string message = "unknown key `";
  message.append(field.key);
  message.append("` in `[build]`");
  Report(kUnknownKey, std::move(message),
         Span(field.key_offset, field.key.size()));
}

Manifest ManifestParser::Run() {
  Manifest manifest;
  std::string_view table;
  uint32_t table_offset = 0;
  bool saw_package = false;

  size_t position = 0;
  while (position <= text_.size()) {
    const size_t newline = text_.find('\n', position);
    const size_t length =
        (newline == std::string_view::npos ? text_.size() : newline) -
        position;
    const std::string_view raw = text_.substr(position, length);
    const auto line_offset = static_cast<uint32_t>(position);
    position = (newline == std::string_view::npos) ? text_.size() + 1
                                                   : newline + 1;

    const std::string_view stripped = StripComment(raw);
    const std::string_view line = Trim(stripped);
    if (line.empty()) {
      continue;
    }
    const auto offset =
        line_offset + static_cast<uint32_t>(line.data() - raw.data());

    if (line.front() == '[') {
      if (line.back() != ']') {
        Report(kSyntaxError, "expected `]` to close the table header",
               Span(offset, line.size()));
        continue;
      }
      table = Trim(line.substr(1, line.size() - 2));
      table_offset = offset;
      seen_.clear();
      if (table == "package") {
        saw_package = true;
      } else if (table != "dependencies" && table != "lints" &&
                 table != "build" && table != "link") {
        std::string message = "unknown table `[";
        message.append(table);
        message.append("]`");
        Report(kUnknownKey, std::move(message), Span(offset, line.size()),
               "known tables are package, dependencies, lints, build and "
               "link");
      }
      continue;
    }

    Field field;
    if (!SplitField(line, offset, &field)) {
      continue;
    }

    if (table.empty()) {
      Report(kSyntaxError, "every key must belong to a table",
             Span(field.key_offset, field.key.size()),
             "start the file with `[package]`");
      continue;
    }

    const std::string key(field.key);
    bool duplicate = false;
    for (const std::string& previous : seen_) {
      if (previous == key) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      std::string message = "duplicate key `";
      message.append(field.key);
      message.append("`");
      Report(kDuplicateKey, std::move(message),
             Span(field.key_offset, field.key.size()));
      continue;
    }
    seen_.push_back(key);

    if (table == "package") {
      ApplyPackageField(field, &manifest);
    } else if (table == "dependencies") {
      ApplyDependencyField(field, &manifest);
    } else if (table == "lints") {
      ApplyLintField(field, &manifest);
    } else if (table == "build") {
      ApplyBuildField(field, &manifest);
    } else if (table == "link") {
      ApplyLinkField(field, &manifest);
    }
  }

  if (!saw_package) {
    Report(kMissingKey, "the manifest has no `[package]` table",
           SourceSpan::Point(file_, 0),
           "every package needs a name and a version");
  } else if (manifest.name.empty()) {
    Report(kMissingKey, "`[package]` has no `name`",
           Span(table_offset, 9));
  }

  return manifest;
}

}  // namespace

Severity Manifest::SeverityFor(std::string_view code,
                               Severity fallback) const {
  for (const LintOverride& entry : lints) {
    if (entry.code == code) {
      return entry.severity;
    }
  }
  return fallback;
}

Manifest ParseManifest(const SourceManager& sources,
                       FileId file,
                       DiagnosticEngine* diagnostics) {
  ManifestParser parser(sources, file, diagnostics);
  return parser.Run();
}

}  // namespace wolang
