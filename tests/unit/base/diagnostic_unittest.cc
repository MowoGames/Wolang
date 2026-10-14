// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/base/diagnostic.h"

#include <string>

#include "harness/test.h"
#include "wolang/base/diagnostic_renderer.h"
#include "wolang/base/source_manager.h"

namespace wolang {
namespace {

WOLANG_TEST(DiagnosticEngine, BuilderEmitsOnDestruction) {
  DiagnosticEngine diagnostics;
  diagnostics.Error("E0101", "mismatched types");
  WOLANG_ASSERT_EQ(diagnostics.diagnostics().size(), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics.diagnostics().front().code, "E0101");
  WOLANG_EXPECT(diagnostics.has_errors());
}

WOLANG_TEST(DiagnosticEngine, ExplicitEmitDoesNotDuplicate) {
  DiagnosticEngine diagnostics;
  diagnostics.Warning("W0101", "unused variable `x`").Emit();
  WOLANG_EXPECT_EQ(diagnostics.diagnostics().size(), size_t{1});
}

WOLANG_TEST(DiagnosticEngine, CountsBySeverity) {
  DiagnosticEngine diagnostics;
  diagnostics.Error("E0101", "one");
  diagnostics.Warning("W0101", "two");
  diagnostics.Warning("W0102", "three");
  diagnostics.Perf("P0101", "four");
  diagnostics.Hint("H0101", "five");

  WOLANG_EXPECT_EQ(diagnostics.Count(Severity::kError), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics.Count(Severity::kWarning), size_t{2});
  WOLANG_EXPECT_EQ(diagnostics.Count(Severity::kPerf), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics.Count(Severity::kHint), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics.error_count(), size_t{1});
}

WOLANG_TEST(DiagnosticEngine, SeverityOverridePromotesAndSilences) {
  DiagnosticEngine diagnostics;
  diagnostics.SetSeverityOverride("P0101", Severity::kError);
  diagnostics.SetSeverityOverride("W0105", Severity::kOff);

  diagnostics.Perf("P0101", "allocation in a hot path");
  diagnostics.Warning("W0105", "float compared with `==`");

  WOLANG_ASSERT_EQ(diagnostics.diagnostics().size(), size_t{1});
  WOLANG_EXPECT_EQ(diagnostics.diagnostics().front().severity,
                   Severity::kError);
}

WOLANG_TEST(DiagnosticEngine, SuppressedBuilderIgnoresLabels) {
  DiagnosticEngine diagnostics;
  diagnostics.SetSeverityOverride("H0101", Severity::kOff);
  {
    DiagnosticBuilder builder = diagnostics.Hint("H0101", "prefer `const`");
    builder.Primary(SourceSpan{FileId{0}, 0, 1}, "here").Note("note");
    WOLANG_EXPECT(builder.is_suppressed());
  }
  WOLANG_EXPECT(diagnostics.diagnostics().empty());
}

WOLANG_TEST(DiagnosticEngine, ClearResetsDiagnosticsButKeepsOverrides) {
  DiagnosticEngine diagnostics;
  diagnostics.SetSeverityOverride("W0105", Severity::kOff);
  diagnostics.Error("E0101", "boom");
  diagnostics.Clear();

  WOLANG_EXPECT(diagnostics.diagnostics().empty());
  diagnostics.Warning("W0105", "still silenced");
  WOLANG_EXPECT(diagnostics.diagnostics().empty());
}

WOLANG_TEST(DiagnosticTest, PrimarySpanPrefersPrimaryLabel) {
  const FileId file{0};
  DiagnosticEngine diagnostics;
  diagnostics.Error("E0101", "mismatched types")
      .Secondary(SourceSpan{file, 0, 1}, "secondary")
      .Primary(SourceSpan{file, 10, 12}, "primary");

  const SourceSpan span = diagnostics.diagnostics().front().PrimarySpan();
  WOLANG_EXPECT_EQ(span.begin, 10u);
}

WOLANG_TEST(DiagnosticTest, SeverityNamesRoundTrip) {
  WOLANG_EXPECT_EQ(SeverityName(Severity::kError), "error");
  WOLANG_EXPECT_EQ(SeverityName(Severity::kWarning), "warning");
  WOLANG_EXPECT_EQ(SeverityName(Severity::kPerf), "perf");
  WOLANG_EXPECT_EQ(SeverityName(Severity::kHint), "hint");

  Severity parsed = Severity::kOff;
  WOLANG_EXPECT(ParseSeverity("warning", &parsed));
  WOLANG_EXPECT_EQ(parsed, Severity::kWarning);
  WOLANG_EXPECT(ParseSeverity("warn", &parsed));
  WOLANG_EXPECT_EQ(parsed, Severity::kWarning);
  WOLANG_EXPECT(!ParseSeverity("loud", &parsed));
  WOLANG_EXPECT_EQ(parsed, Severity::kWarning);
}

WOLANG_TEST(DiagnosticRenderer, RendersLabelsNotesAndLocation) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile(
      "combat/turret.wo",
      "fun Fire() {\n  const t = target\n  wait Wait(0.5s)\n}\n");

  DiagnosticEngine diagnostics;
  diagnostics.Warning("W0107", "actor reference `t` held across `wait`")
      .Secondary(SourceSpan{file, 21, 22}, "`t` captured here")
      .Primary(SourceSpan{file, 34, 49}, "`t` may be destroyed while waiting")
      .Note("re-check liveness after the wait");

  const std::string expected =
      "warning[W0107]: actor reference `t` held across `wait`\n"
      " --> combat/turret.wo:3:3\n"
      "  |\n"
      "2 |   const t = target\n"
      "  |         - `t` captured here\n"
      "3 |   wait Wait(0.5s)\n"
      "  |   ^^^^^^^^^^^^^^^ `t` may be destroyed while waiting\n"
      "  |\n"
      "  = note: re-check liveness after the wait\n";

  WOLANG_EXPECT_EQ(
      RenderDiagnostic(sources, diagnostics.diagnostics().front()), expected);
}

WOLANG_TEST(DiagnosticRenderer, RendersFixWithAppliedEdit) {
  SourceManager sources;
  const FileId file =
      sources.AddVirtualFile("a.wo", "if x != null { x.Jump() }\n");

  DiagnosticEngine diagnostics;
  diagnostics.Hint("H0103", "null check can be an optional call")
      .Primary(SourceSpan{file, 0, 25})
      .Suggest("use `?.`", SourceSpan{file, 0, 25}, "x?.Jump()");

  const std::string expected =
      "hint[H0103]: null check can be an optional call\n"
      " --> a.wo:1:1\n"
      "  |\n"
      "1 | if x != null { x.Jump() }\n"
      "  | ^^^^^^^^^^^^^^^^^^^^^^^^^\n"
      "  |\n"
      "  = help: use `?.`\n"
      "1 | x?.Jump()\n";

  WOLANG_EXPECT_EQ(
      RenderDiagnostic(sources, diagnostics.diagnostics().front()), expected);
}

WOLANG_TEST(DiagnosticRenderer, MarksMultiLineSpansAsContinuing) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile("a.wo", "const s = \"abc\ndef\n");

  DiagnosticEngine diagnostics;
  diagnostics.Error("E0001", "unterminated string literal")
      .Primary(SourceSpan{file, 8, 16}, "literal starts here");

  const std::string rendered =
      RenderDiagnostic(sources, diagnostics.diagnostics().front());
  WOLANG_EXPECT(rendered.find("^^^^...") != std::string::npos);
}

WOLANG_TEST(DiagnosticRenderer, ExpandsTabsWhenAligningMarkers) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile("a.wo", "\tconst x = 1\n");

  DiagnosticEngine diagnostics;
  diagnostics.Error("E0105", "cannot assign to `x`")
      .Primary(SourceSpan{file, 7, 8}, "immutable binding");

  const std::string rendered =
      RenderDiagnostic(sources, diagnostics.diagnostics().front());
  // The tab becomes four spaces, so `x` sits in display column 11.
  WOLANG_EXPECT(rendered.find("1 |     const x = 1\n") != std::string::npos);
  WOLANG_EXPECT(rendered.find("  |           ^ immutable binding\n") !=
                std::string::npos);
}

WOLANG_TEST(DiagnosticRenderer, ReportsLabelsFromOtherFilesAsNotes) {
  SourceManager sources;
  const FileId primary = sources.AddVirtualFile("a.wo", "fun f() {}\n");
  const FileId other = sources.AddVirtualFile("b.wo", "fun f() {}\n");

  DiagnosticEngine diagnostics;
  diagnostics.Error("E0401", "duplicate definition of `f`")
      .Primary(SourceSpan{primary, 3, 4}, "redefined here")
      .Secondary(SourceSpan{other, 3, 4}, "first defined here");

  const std::string rendered =
      RenderDiagnostic(sources, diagnostics.diagnostics().front());
  WOLANG_EXPECT(rendered.find("note: also at b.wo:1:4: first defined here") !=
                std::string::npos);
}

WOLANG_TEST(DiagnosticRenderer, RendersWithoutLabels) {
  SourceManager sources;
  DiagnosticEngine diagnostics;
  diagnostics.Error("E0901", "comptime assertion failed")
      .Note("check the module configuration");

  const std::string expected =
      "error[E0901]: comptime assertion failed\n"
      "  = note: check the module configuration\n";
  WOLANG_EXPECT_EQ(
      RenderDiagnostic(sources, diagnostics.diagnostics().front()), expected);
}

WOLANG_TEST(DiagnosticRenderer, SummaryCountsAndPluralizes) {
  DiagnosticEngine diagnostics;
  WOLANG_EXPECT(RenderSummary(diagnostics).empty());

  diagnostics.Error("E0101", "one");
  diagnostics.Warning("W0101", "two");
  diagnostics.Warning("W0102", "three");
  WOLANG_EXPECT_EQ(RenderSummary(diagnostics), "1 error, 2 warnings");
}

WOLANG_TEST(DiagnosticRenderer, ColorOptionAddsEscapeSequences) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile("a.wo", "const x = 1\n");
  DiagnosticEngine diagnostics;
  diagnostics.Error("E0101", "mismatched types")
      .Primary(SourceSpan{file, 4, 5});

  RenderOptions options;
  options.color = true;
  const std::string rendered =
      RenderDiagnostic(sources, diagnostics.diagnostics().front(), options);
  WOLANG_EXPECT(rendered.find("\x1b[1;31m") != std::string::npos);
  WOLANG_EXPECT(rendered.find("\x1b[0m") != std::string::npos);
}

}  // namespace
}  // namespace wolang
