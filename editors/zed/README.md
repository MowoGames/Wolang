# Wolang for Zed

A dev extension: `zed: install dev extension` and point it at this
directory. It starts `wolang lsp`, which is where every answer comes
from -- diagnostics, hover, go to definition, symbols, formatting and
completion.

Zed wants a Tree-sitter grammar for highlighting, which this does not
have yet; without one the editor still shows everything the language
server says. The grammar is tracked as its own piece of work: a
Tree-sitter parser is a second parser for the same language, so it has
to be generated from the real grammar rather than written twice.
