// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/base/diagnostic.h"

#include <algorithm>
#include <utility>

namespace wolang {

std::string_view SeverityName(Severity severity) {
  switch (severity) {
    case Severity::kOff:
      return "off";
    case Severity::kHint:
      return "hint";
    case Severity::kPerf:
      return "perf";
    case Severity::kWarning:
      return "warning";
    case Severity::kError:
      return "error";
  }
  return "error";
}

bool ParseSeverity(std::string_view name, Severity* out) {
  static constexpr struct {
    std::string_view name;
    Severity severity;
  } kNames[] = {
      {"off", Severity::kOff},         {"hint", Severity::kHint},
      {"perf", Severity::kPerf},       {"warning", Severity::kWarning},
      {"warn", Severity::kWarning},    {"error", Severity::kError},
  };

  for (const auto& entry : kNames) {
    if (entry.name == name) {
      if (out) {
        *out = entry.severity;
      }
      return true;
    }
  }
  return false;
}

SourceSpan Diagnostic::PrimarySpan() const {
  for (const auto& label : labels) {
    if (label.is_primary) {
      return label.span;
    }
  }
  if (!labels.empty()) {
    return labels.front().span;
  }
  return SourceSpan{};
}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticEngine* engine,
                                     Diagnostic diagnostic)
    : engine_(engine),
      diagnostic_(std::move(diagnostic)),
      suppressed_(diagnostic_.severity == Severity::kOff) {}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticBuilder&& other) noexcept
    : engine_(other.engine_),
      diagnostic_(std::move(other.diagnostic_)),
      suppressed_(other.suppressed_),
      emitted_(other.emitted_) {
  // The moved-from builder must not emit a second copy from its destructor.
  other.emitted_ = true;
  other.engine_ = nullptr;
}

DiagnosticBuilder::~DiagnosticBuilder() {
  Emit();
}

DiagnosticBuilder& DiagnosticBuilder::Primary(SourceSpan span,
                                              std::string message) {
  if (!suppressed_) {
    diagnostic_.labels.push_back(
        DiagnosticLabel{span, std::move(message), true});
  }
  return *this;
}

DiagnosticBuilder& DiagnosticBuilder::Secondary(SourceSpan span,
                                                std::string message) {
  if (!suppressed_) {
    diagnostic_.labels.push_back(
        DiagnosticLabel{span, std::move(message), false});
  }
  return *this;
}

DiagnosticBuilder& DiagnosticBuilder::Note(std::string text) {
  if (!suppressed_) {
    diagnostic_.notes.push_back(std::move(text));
  }
  return *this;
}

DiagnosticBuilder& DiagnosticBuilder::Suggest(std::string message,
                                              SourceSpan span,
                                              std::string new_text) {
  if (suppressed_) {
    return *this;
  }
  Fix fix;
  fix.message = std::move(message);
  fix.edits.push_back(FixEdit{span, std::move(new_text)});
  diagnostic_.fixes.push_back(std::move(fix));
  return *this;
}

DiagnosticBuilder& DiagnosticBuilder::AddFix(Fix fix) {
  if (!suppressed_) {
    diagnostic_.fixes.push_back(std::move(fix));
  }
  return *this;
}

void DiagnosticBuilder::Emit() {
  if (emitted_) {
    return;
  }
  emitted_ = true;
  if (suppressed_ || engine_ == nullptr) {
    return;
  }
  engine_->Add(std::move(diagnostic_));
}

DiagnosticEngine::DiagnosticEngine() = default;
DiagnosticEngine::~DiagnosticEngine() = default;

DiagnosticBuilder DiagnosticEngine::Report(Severity severity,
                                           std::string_view code,
                                           std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = SeverityFor(code, severity);
  diagnostic.code = std::string(code);
  diagnostic.message = std::move(message);
  return DiagnosticBuilder(this, std::move(diagnostic));
}

DiagnosticBuilder DiagnosticEngine::Error(std::string_view code,
                                          std::string message) {
  return Report(Severity::kError, code, std::move(message));
}

DiagnosticBuilder DiagnosticEngine::Warning(std::string_view code,
                                            std::string message) {
  return Report(Severity::kWarning, code, std::move(message));
}

DiagnosticBuilder DiagnosticEngine::Perf(std::string_view code,
                                         std::string message) {
  return Report(Severity::kPerf, code, std::move(message));
}

DiagnosticBuilder DiagnosticEngine::Hint(std::string_view code,
                                         std::string message) {
  return Report(Severity::kHint, code, std::move(message));
}

void DiagnosticEngine::SetSeverityOverride(std::string_view code,
                                           Severity severity) {
  severity_overrides_[std::string(code)] = severity;
}

void DiagnosticEngine::ClearSeverityOverrides() {
  severity_overrides_.clear();
}

Severity DiagnosticEngine::SeverityFor(std::string_view code,
                                       Severity fallback) const {
  auto it = severity_overrides_.find(std::string(code));
  if (it == severity_overrides_.end()) {
    return fallback;
  }
  return it->second;
}

size_t DiagnosticEngine::Count(Severity severity) const {
  return static_cast<size_t>(
      std::count_if(diagnostics_.begin(), diagnostics_.end(),
                    [severity](const Diagnostic& diagnostic) {
                      return diagnostic.severity == severity;
                    }));
}

void DiagnosticEngine::Clear() {
  diagnostics_.clear();
}

void DiagnosticEngine::Add(Diagnostic diagnostic) {
  diagnostics_.push_back(std::move(diagnostic));
}

}  // namespace wolang
