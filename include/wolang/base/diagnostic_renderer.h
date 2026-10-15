// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_BASE_DIAGNOSTIC_RENDERER_H_
#define WOLANG_BASE_DIAGNOSTIC_RENDERER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"

namespace wolang {

struct RenderOptions {
  // Emit ANSI escape sequences. Callers should enable this only for a tty.
  bool color = false;
  // Render suggested fixes as "help:" sections.
  bool show_fixes = true;
  // Columns a tab advances to when echoing source lines.
  uint32_t tab_width = 4;
};

// Renders one diagnostic in the terminal format used by `wolang check`:
//
//   warning[W0107]: actor reference `t` held across `await`
//     --> combat/turret.wo:24:17
//      |
//   21 |     if let t = target {
//      |            - `t` captured here
//   24 |         await Wait(0.5s)
//      |         ^^^^^^^^^^^^^^^^ `t` may be destroyed while waiting
//      |
//      = help: re-check liveness after the await
//
// The returned string always ends with a newline.
std::string RenderDiagnostic(const SourceManager& sources,
                             const Diagnostic& diagnostic,
                             const RenderOptions& options = {});

// Renders every diagnostic in order, separated by a blank line.
std::string RenderDiagnostics(const SourceManager& sources,
                              const std::vector<Diagnostic>& diagnostics,
                              const RenderOptions& options = {});

// Returns a one-line tally such as "2 errors, 1 warning" or an empty string
// when nothing was reported.
std::string RenderSummary(const DiagnosticEngine& diagnostics,
                          const RenderOptions& options = {});

}  // namespace wolang

#endif  // WOLANG_BASE_DIAGNOSTIC_RENDERER_H_
