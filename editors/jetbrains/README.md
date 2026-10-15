# Wolang in a JetBrains IDE

JetBrains has no Wolang plugin yet. It does have LSP4IJ, which makes any
language server a first-class one, and that is what to use -- a plugin
written in Kotlin would reimplement what `wolang lsp` already answers.

1. Settings → Plugins → Marketplace → install **LSP4IJ**.
2. Settings → Languages & Frameworks → Language Servers → **+**.
3. Fill in:
   - **Name**: `Wolang`
   - **Command**: `wolang lsp`
   - **Mappings → File name patterns**: `*.wo`, with Language ID `wolang`
4. Settings → Editor → File Types → **+** → `Wolang`, line comment `//`,
   block comment `/*` `*/`, keywords from `docs/spec.md`, extension `*.wo`.

Diagnostics, hover, go to definition, symbols, formatting and completion
work through LSP4IJ. A native plugin is tracked as its own piece of work;
what it would add is the IDE's own refactorings, which need a model of
the language inside the IDE rather than over a protocol.
