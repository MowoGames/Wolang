// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_FORMAT_FORMATTER_H_
#define WOLANG_FORMAT_FORMATTER_H_

#include <cstdint>
#include <string>

#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast.h"
#include "wolang/syntax/token.h"

namespace wolang {

struct FormatOptions {
  uint32_t column_limit = 80;
  uint32_t indent_width = 2;
};

// Renders a syntax tree back to source in the one canonical style.
//
// There are no style options beyond the margin: a single format means no
// project ever argues about it, and `wolang fmt --check` is a build step
// rather than a conversation.
//
// The formatter works from the tree, not from the original text, so it is
// idempotent by construction -- with one exception it has to handle by hand:
// comments are not part of the tree, and are re-attached from the token
// stream's comment table by position.
//
// Formatting a file with syntax errors is not meaningful, so callers should
// check for diagnostics first; given a tree with error nodes, the formatter
// emits what it can and leaves the error markers as comments.
std::string Format(const SourceManager& sources,
                   const StringInterner& interner,
                   const TokenStream& tokens,
                   const Ast& ast,
                   const FormatOptions& options = {});

}  // namespace wolang

#endif  // WOLANG_FORMAT_FORMATTER_H_
