# Editors

Every editor here talks to the same server: `wolang lsp`, which is the
compiler answering. Diagnostics, hover, go to definition, document
symbols, formatting and completion come from there, so nothing in this
directory has to be kept in step with the language -- only the
highlighting, which is a regular expression's idea of Wolang and exists
so that a file looks right before the server has answered.

| Editor | What to do |
| --- | --- |
| VS Code, Cursor, Windsurf, VSCodium | `cd vscode && npm install && npm run compile`, then `code --install-extension` the packaged `.vsix`, or press F5 to run it from source. |
| Zed | `zed: install dev extension`, pointed at `zed/`. |
| Neovim 0.10+ | Copy `neovim/wolang.lua` into `~/.config/nvim/lua/` and `require('wolang').setup()`; `neovim/syntax/wolang.vim` into `~/.config/nvim/syntax/`. |
| Vim 8 with a language client | `neovim/syntax/wolang.vim`, and point your client at `wolang lsp`. |
| Sublime Text | Install the LSP package; copy both files from `sublime/` into `Packages/User/`. |
| Emacs 29+ | `(load "wolang-mode.el")`; Eglot picks up `wolang lsp` from it. |
| JetBrains IDEs (IntelliJ, CLion, Rider) | Install [LSP4IJ](https://plugins.jetbrains.com/plugin/23257-lsp4ij) and add a server with the command `wolang lsp` for `*.wo`. `jetbrains/README.md` has the exact fields. |
| Helix | `helix/languages.toml` -- append it to `~/.config/helix/languages.toml`. |
| Anything else with LSP | `wolang lsp` over stdio. It needs no arguments and no configuration. |

## What the server answers

- `textDocument/publishDiagnostics` -- the same errors `wolang check` gives.
- `textDocument/hover` -- the type of what is under the cursor, and its
  documentation comment.
- `textDocument/definition` -- where a name was declared.
- `textDocument/documentSymbol` -- the declarations in a file.
- `textDocument/formatting` -- `wolang fmt`, so a file formatted by the
  editor and one formatted by the tool are the same file.
- `textDocument/completion` -- what is in scope, and the members of what
  is before the dot.

## Formatting a buffer that is not saved

`wolang fmt --stdin` reads a file from standard input and writes the
formatted text back. Text that does not parse comes back unchanged
rather than as an error: a buffer mid-edit is the normal case, and
losing it would be unforgivable.
