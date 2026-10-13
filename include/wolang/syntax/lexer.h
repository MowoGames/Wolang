// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SYNTAX_LEXER_H_
#define WOLANG_SYNTAX_LEXER_H_

#include "wolang/base/diagnostic.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/token.h"

namespace wolang {

struct LexOptions {
  // Record the spans of ordinary comments in TokenStream::comments. The
  // formatter needs them; the parser does not.
  bool keep_comments = true;
};

// Converts one source file into tokens.
//
// Lexing never fails: malformed input produces a diagnostic plus a recovery
// token flagged with TokenFlags::kHasError, so that a broken literal in the
// middle of a file still lets the parser see the structure around it. The
// returned stream always ends with a TokenKind::kEndOfFile token.
TokenStream Lex(const SourceManager& sources,
                FileId file,
                StringInterner* interner,
                DiagnosticEngine* diagnostics,
                const LexOptions& options = {});

}  // namespace wolang

#endif  // WOLANG_SYNTAX_LEXER_H_
