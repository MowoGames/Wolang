// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SYNTAX_PARSER_H_
#define WOLANG_SYNTAX_PARSER_H_

#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast.h"
#include "wolang/syntax/token.h"

namespace wolang {

struct ParseOptions {
  // Stop after this many errors in one file. Beyond it the parser skips to the
  // end rather than producing noise from a hopelessly broken file.
  uint32_t error_limit = 64;
};

// Parses a token stream into a syntax tree.
//
// Parsing never fails: an unparsable construct becomes an error node, a
// diagnostic is reported, and the parser resynchronizes on the next
// declaration, statement or closing brace. The returned tree therefore always
// covers whatever was well formed, which is what the language server needs
// while a file is half typed.
Ast Parse(const SourceManager& sources,
          const TokenStream& tokens,
          StringInterner* interner,
          DiagnosticEngine* diagnostics,
          const ParseOptions& options = {});

}  // namespace wolang

#endif  // WOLANG_SYNTAX_PARSER_H_
