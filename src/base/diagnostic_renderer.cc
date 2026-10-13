// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/base/diagnostic_renderer.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

namespace wolang {
namespace {

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kBlue = "\x1b[1;34m";

std::string_view SeverityColor(Severity severity) {
  switch (severity) {
    case Severity::kError:
      return "\x1b[1;31m";
    case Severity::kWarning:
      return "\x1b[1;33m";
    case Severity::kPerf:
      return "\x1b[1;36m";
    case Severity::kHint:
      return "\x1b[1;34m";
    case Severity::kOff:
      return "";
  }
  return "";
}

std::string Paint(std::string_view text,
                  std::string_view color,
                  const RenderOptions& options) {
  if (!options.color || color.empty()) {
    return std::string(text);
  }
  std::string result(color);
  result.append(text);
  result.append(kReset);
  return result;
}

std::string FormatUint(uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%u", value);
  return std::string(buffer);
}

size_t DigitCount(uint32_t value) {
  size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

// A source line prepared for display: tabs expanded, plus a mapping from byte
// offset inside the line to display column so that markers line up under
// multi-byte characters and tabs.
struct DisplayLine {
  std::string text;
  std::vector<uint32_t> column_of_byte;

  uint32_t ColumnAt(size_t byte_offset) const {
    if (column_of_byte.empty()) {
      return 0;
    }
    const size_t index = std::min(byte_offset, column_of_byte.size() - 1);
    return column_of_byte[index];
  }
};

bool IsUtf8Continuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

DisplayLine MakeDisplayLine(std::string_view source, uint32_t tab_width) {
  DisplayLine line;
  line.column_of_byte.reserve(source.size() + 1);
  uint32_t column = 0;
  for (char c : source) {
    line.column_of_byte.push_back(column);
    if (c == '\t') {
      const uint32_t width =
          tab_width == 0 ? 1 : tab_width - (column % tab_width);
      line.text.append(width, ' ');
      column += width;
    } else if (c == '\r') {
      // Dropped: rendering adds its own line breaks.
    } else {
      line.text.push_back(c);
      if (!IsUtf8Continuation(c)) {
        ++column;
      }
    }
  }
  line.column_of_byte.push_back(column);
  return line;
}

// One label resolved to a single line of a single file.
struct ResolvedLabel {
  uint32_t line = 1;
  uint32_t begin_byte = 0;
  uint32_t end_byte = 0;
  bool is_primary = false;
  bool continues_on_next_line = false;
  const std::string* message = nullptr;
};

void AppendGutterBar(std::string* out,
                     size_t gutter_width,
                     const RenderOptions& options) {
  std::string bar(gutter_width + 1, ' ');
  bar.push_back('|');
  out->append(Paint(bar, kBlue, options));
  out->push_back('\n');
}

void AppendSourceLine(std::string* out,
                      uint32_t line_number,
                      size_t gutter_width,
                      const DisplayLine& line,
                      const RenderOptions& options) {
  const std::string number = FormatUint(line_number);
  std::string prefix(gutter_width - number.size(), ' ');
  prefix.append(number);
  prefix.append(" |");
  out->append(Paint(prefix, kBlue, options));
  out->push_back(' ');
  out->append(line.text);
  out->push_back('\n');
}

void AppendMarkerLine(std::string* out,
                      size_t gutter_width,
                      const DisplayLine& line,
                      const ResolvedLabel& label,
                      Severity severity,
                      const RenderOptions& options) {
  std::string prefix(gutter_width + 1, ' ');
  prefix.push_back('|');
  out->append(Paint(prefix, kBlue, options));
  out->push_back(' ');

  const uint32_t start = line.ColumnAt(label.begin_byte);
  uint32_t stop = line.ColumnAt(label.end_byte);
  if (stop <= start) {
    stop = start + 1;
  }
  out->append(start, ' ');

  const char marker = label.is_primary ? '^' : '-';
  std::string markers(stop - start, marker);
  if (label.continues_on_next_line) {
    markers.append("...");
  }

  std::string_view color =
      label.is_primary ? SeverityColor(severity) : std::string_view(kBlue);
  std::string painted = Paint(markers, color, options);
  if (label.message && !label.message->empty()) {
    painted.push_back(' ');
    painted.append(Paint(*label.message, color, options));
  }
  out->append(painted);
  out->push_back('\n');
}

}  // namespace

std::string RenderDiagnostic(const SourceManager& sources,
                             const Diagnostic& diagnostic,
                             const RenderOptions& options) {
  std::string out;

  // Header: "error[E0001]: message".
  std::string headline(SeverityName(diagnostic.severity));
  if (!diagnostic.code.empty()) {
    headline.push_back('[');
    headline.append(diagnostic.code);
    headline.push_back(']');
  }
  out.append(Paint(headline, SeverityColor(diagnostic.severity), options));
  out.append(": ");
  out.append(Paint(diagnostic.message, kBold, options));
  out.push_back('\n');

  const SourceSpan primary = diagnostic.PrimarySpan();
  if (!primary.file.is_valid()) {
    for (const std::string& note : diagnostic.notes) {
      out.append("  = ");
      out.append(Paint("note", kBold, options));
      out.append(": ");
      out.append(note);
      out.push_back('\n');
    }
    return out;
  }

  // Resolve the labels that live in the primary file; labels pointing at other
  // files are reported as notes so that nothing is silently dropped.
  std::vector<ResolvedLabel> resolved;
  std::vector<std::string> extra_notes;
  for (const auto& label : diagnostic.labels) {
    if (!label.span.file.is_valid()) {
      continue;
    }
    if (label.span.file != primary.file) {
      std::string note = "also at ";
      note.append(sources.FormatPosition(label.span.file, label.span.begin));
      if (!label.message.empty()) {
        note.append(": ");
        note.append(label.message);
      }
      extra_notes.push_back(std::move(note));
      continue;
    }

    const LineColumn begin = sources.Position(label.span.file, label.span.begin);
    const LineColumn end = sources.Position(label.span.file, label.span.end);
    const uint32_t line_start =
        sources.LineStartOffset(label.span.file, begin.line);
    const std::string_view line_text =
        sources.LineText(label.span.file, begin.line);

    ResolvedLabel item;
    item.line = begin.line;
    item.begin_byte = label.span.begin - line_start;
    item.is_primary = label.is_primary;
    item.message = &label.message;
    if (end.line != begin.line) {
      item.end_byte = static_cast<uint32_t>(line_text.size());
      item.continues_on_next_line = true;
    } else {
      item.end_byte = label.span.end - line_start;
    }
    resolved.push_back(item);
  }

  std::stable_sort(resolved.begin(), resolved.end(),
                   [](const ResolvedLabel& lhs, const ResolvedLabel& rhs) {
                     if (lhs.line != rhs.line) {
                       return lhs.line < rhs.line;
                     }
                     return lhs.begin_byte < rhs.begin_byte;
                   });

  uint32_t max_line = sources.Position(primary.file, primary.begin).line;
  for (const auto& label : resolved) {
    max_line = std::max(max_line, label.line);
  }
  const size_t gutter_width = DigitCount(max_line);

  // Location line: "  --> path:line:column".
  const LineColumn primary_position =
      sources.Position(primary.file, primary.begin);
  std::string arrow(gutter_width, ' ');
  arrow.append("--> ");
  out.append(Paint(arrow, kBlue, options));
  out.append(sources.Path(primary.file));
  out.push_back(':');
  out.append(FormatUint(primary_position.line));
  out.push_back(':');
  out.append(FormatUint(primary_position.column));
  out.push_back('\n');

  AppendGutterBar(&out, gutter_width, options);

  uint32_t previous_line = 0;
  for (size_t i = 0; i < resolved.size();) {
    const uint32_t line_number = resolved[i].line;
    if (previous_line != 0 && line_number > previous_line + 1) {
      out.append(Paint("...", kBlue, options));
      out.push_back('\n');
    }

    const DisplayLine line = MakeDisplayLine(
        sources.LineText(primary.file, line_number), options.tab_width);
    AppendSourceLine(&out, line_number, gutter_width, line, options);
    while (i < resolved.size() && resolved[i].line == line_number) {
      AppendMarkerLine(&out, gutter_width, line, resolved[i],
                       diagnostic.severity, options);
      ++i;
    }
    previous_line = line_number;
  }

  AppendGutterBar(&out, gutter_width, options);

  std::string note_prefix(gutter_width + 1, ' ');
  note_prefix.append("= ");
  for (const std::string& note : diagnostic.notes) {
    out.append(Paint(note_prefix, kBlue, options));
    out.append(Paint("note", kBold, options));
    out.append(": ");
    out.append(note);
    out.push_back('\n');
  }
  for (const std::string& note : extra_notes) {
    out.append(Paint(note_prefix, kBlue, options));
    out.append(Paint("note", kBold, options));
    out.append(": ");
    out.append(note);
    out.push_back('\n');
  }

  if (!options.show_fixes) {
    return out;
  }

  for (const Fix& fix : diagnostic.fixes) {
    out.append(Paint(note_prefix, kBlue, options));
    out.append(Paint("help", kBold, options));
    out.append(": ");
    out.append(fix.message);
    out.push_back('\n');

    // Single-line replacements are shown with the edit already applied, which
    // is the form editors offer as a quick fix.
    if (fix.edits.size() != 1) {
      continue;
    }
    const FixEdit& edit = fix.edits.front();
    if (!edit.span.file.is_valid() || edit.span.file != primary.file) {
      continue;
    }
    const LineColumn begin = sources.Position(edit.span.file, edit.span.begin);
    const LineColumn end = sources.Position(edit.span.file, edit.span.end);
    if (begin.line != end.line) {
      continue;
    }
    const uint32_t line_start =
        sources.LineStartOffset(edit.span.file, begin.line);
    const std::string_view line_text =
        sources.LineText(edit.span.file, begin.line);
    std::string patched(line_text.substr(0, edit.span.begin - line_start));
    patched.append(edit.new_text);
    const size_t tail = edit.span.end - line_start;
    if (tail < line_text.size()) {
      patched.append(line_text.substr(tail));
    }

    const DisplayLine line = MakeDisplayLine(patched, options.tab_width);
    AppendSourceLine(&out, begin.line, gutter_width, line, options);
  }

  return out;
}

std::string RenderDiagnostics(const SourceManager& sources,
                              const std::vector<Diagnostic>& diagnostics,
                              const RenderOptions& options) {
  std::string out;
  for (size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0) {
      out.push_back('\n');
    }
    out.append(RenderDiagnostic(sources, diagnostics[i], options));
  }
  return out;
}

std::string RenderSummary(const DiagnosticEngine& diagnostics,
                          const RenderOptions& options) {
  struct Bucket {
    Severity severity;
    std::string_view singular;
    std::string_view plural;
  };
  static constexpr Bucket kBuckets[] = {
      {Severity::kError, "error", "errors"},
      {Severity::kWarning, "warning", "warnings"},
      {Severity::kPerf, "perf note", "perf notes"},
      {Severity::kHint, "hint", "hints"},
  };

  std::string out;
  for (const Bucket& bucket : kBuckets) {
    const size_t count = diagnostics.Count(bucket.severity);
    if (count == 0) {
      continue;
    }
    if (!out.empty()) {
      out.append(", ");
    }
    std::string text = FormatUint(static_cast<uint32_t>(count));
    text.push_back(' ');
    text.append(count == 1 ? bucket.singular : bucket.plural);
    out.append(Paint(text, SeverityColor(bucket.severity), options));
  }
  return out;
}

}  // namespace wolang
