// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_BASE_DIAGNOSTIC_H_
#define WOLANG_BASE_DIAGNOSTIC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wolang/base/source_manager.h"

namespace wolang {

// How much a diagnostic matters. The order is meaningful: configuration can
// only ever be compared with >= to decide whether something is reported.
//
//   kOff      suppressed by configuration
//   kHint     style or simplification advice
//   kPerf     a likely performance problem
//   kWarning  a likely bug
//   kError    blocks compilation and hot reload
enum class Severity : uint8_t {
  kOff = 0,
  kHint,
  kPerf,
  kWarning,
  kError,
};

// Returns "error", "warning", "perf", "hint" or "off".
std::string_view SeverityName(Severity severity);

// Parses a severity name as accepted in build configuration. Returns false for
// unknown names and leaves |out| untouched.
bool ParseSeverity(std::string_view name, Severity* out);

// A span of source called out by a diagnostic. Exactly one label should be
// primary; it decides the reported position.
struct DiagnosticLabel {
  SourceSpan span;
  std::string message;
  bool is_primary = false;

  friend bool operator==(const DiagnosticLabel& lhs,
                         const DiagnosticLabel& rhs) {
    return lhs.span == rhs.span && lhs.message == rhs.message &&
           lhs.is_primary == rhs.is_primary;
  }
};

// One text replacement of a machine applicable fix.
struct FixEdit {
  SourceSpan span;
  std::string new_text;

  friend bool operator==(const FixEdit& lhs, const FixEdit& rhs) {
    return lhs.span == rhs.span && lhs.new_text == rhs.new_text;
  }
};

// A suggested change. Fixes are offered to the editor and to `wolang fix`.
struct Fix {
  std::string message;
  std::vector<FixEdit> edits;

  friend bool operator==(const Fix& lhs, const Fix& rhs) {
    return lhs.message == rhs.message && lhs.edits == rhs.edits;
  }
};

struct Diagnostic {
  Severity severity = Severity::kError;
  // Stable identifier such as "E0101". Used for configuration and for
  // `@allow(...)` suppression.
  std::string code;
  std::string message;
  std::vector<DiagnosticLabel> labels;
  std::vector<std::string> notes;
  std::vector<Fix> fixes;

  // Returns the span of the primary label, or of the first label, or an
  // invalid span when the diagnostic has no labels.
  SourceSpan PrimarySpan() const;

  // Two diagnostics are the same when they would print the same, positions
  // included. That is what lets a query holding diagnostics be compared, so
  // an edit that produces the same messages in the same places leaves
  // everything downstream of it green.
  friend bool operator==(const Diagnostic& lhs, const Diagnostic& rhs) {
    return lhs.severity == rhs.severity && lhs.code == rhs.code &&
           lhs.message == rhs.message && lhs.labels == rhs.labels &&
           lhs.notes == rhs.notes && lhs.fixes == rhs.fixes;
  }
};

class DiagnosticEngine;

// Builds one diagnostic with a chained call sequence and hands it to the
// engine. The diagnostic is emitted by Emit(), or by the destructor if the
// caller never calls it:
//
//   diags.Error("E0001", "unterminated string literal")
//       .Primary(span, "literal starts here")
//       .Note("string literals may not span lines");
class DiagnosticBuilder {
 public:
  DiagnosticBuilder(const DiagnosticBuilder&) = delete;
  DiagnosticBuilder& operator=(const DiagnosticBuilder&) = delete;
  DiagnosticBuilder(DiagnosticBuilder&& other) noexcept;
  DiagnosticBuilder& operator=(DiagnosticBuilder&&) = delete;
  ~DiagnosticBuilder();

  DiagnosticBuilder& Primary(SourceSpan span, std::string message = {});
  DiagnosticBuilder& Secondary(SourceSpan span, std::string message = {});
  DiagnosticBuilder& Note(std::string text);
  DiagnosticBuilder& Suggest(std::string message,
                             SourceSpan span,
                             std::string new_text);
  DiagnosticBuilder& AddFix(Fix fix);

  // Hands the diagnostic to the engine. Further calls are no-ops.
  void Emit();

  bool is_suppressed() const { return suppressed_; }

 private:
  friend class DiagnosticEngine;

  DiagnosticBuilder(DiagnosticEngine* engine, Diagnostic diagnostic);

  DiagnosticEngine* engine_ = nullptr;
  Diagnostic diagnostic_;
  bool suppressed_ = false;
  bool emitted_ = false;
};

// Collects diagnostics for one compilation and applies per-code severity
// configuration. The engine neither sorts nor prints; rendering lives in
// diagnostic_renderer.h.
class DiagnosticEngine {
 public:
  DiagnosticEngine();
  ~DiagnosticEngine();

  DiagnosticEngine(const DiagnosticEngine&) = delete;
  DiagnosticEngine& operator=(const DiagnosticEngine&) = delete;

  DiagnosticBuilder Report(Severity severity,
                           std::string_view code,
                           std::string message);
  DiagnosticBuilder Error(std::string_view code, std::string message);
  DiagnosticBuilder Warning(std::string_view code, std::string message);
  DiagnosticBuilder Perf(std::string_view code, std::string message);
  DiagnosticBuilder Hint(std::string_view code, std::string message);

  // Overrides the severity of |code|, including Severity::kOff to silence it.
  // This is how module configuration promotes lints to errors.
  void SetSeverityOverride(std::string_view code, Severity severity);
  void ClearSeverityOverrides();

  // Returns the configured severity for |code|, or |fallback| when the code
  // has no override.
  Severity SeverityFor(std::string_view code, Severity fallback) const;

  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

  size_t Count(Severity severity) const;
  size_t error_count() const { return Count(Severity::kError); }
  bool has_errors() const { return error_count() > 0; }

  void Clear();

 private:
  friend class DiagnosticBuilder;

  void Add(Diagnostic diagnostic);

  std::vector<Diagnostic> diagnostics_;
  std::unordered_map<std::string, Severity> severity_overrides_;
};

}  // namespace wolang

#endif  // WOLANG_BASE_DIAGNOSTIC_H_
