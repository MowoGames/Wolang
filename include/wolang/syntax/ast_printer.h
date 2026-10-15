// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_SYNTAX_AST_PRINTER_H_
#define WOLANG_SYNTAX_AST_PRINTER_H_

#include <string>

#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast.h"

namespace wolang {

struct AstPrintOptions {
  // Append `@line:column` to every node.
  bool include_spans = false;
  uint32_t indent_width = 2;
};

// Renders a syntax tree as an indented s-expression:
//
//   (file
//     (import engine.gameplay)
//     (class Turret
//       (attr Spawnable)
//       (extends (type Actor))
//       (var range
//         (type float)
//         (init (float 15)))))
//
// The format is the golden form used by the parser tests, so it is stable on
// purpose: changing it means updating those tests deliberately.
//
// |sources| is only needed to turn offsets into line and column numbers, so it
// may be null when `include_spans` is false. That is what lets the item tree
// render an expression canonically without a source manager in reach.
std::string PrintAst(const SourceManager* sources,
                     const StringInterner& interner,
                     const Ast& ast,
                     const AstPrintOptions& options = {});

// Renders one subtree. Handy in tests that only care about an expression.
std::string PrintExpr(const SourceManager* sources,
                      const StringInterner& interner,
                      const Ast& ast,
                      ExprId expr,
                      const AstPrintOptions& options = {});

}  // namespace wolang

#endif  // WOLANG_SYNTAX_AST_PRINTER_H_
