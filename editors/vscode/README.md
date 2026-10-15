# Wolang for Visual Studio Code

<img src="https://raw.githubusercontent.com/MowoGames/Wolang/alpha/assets/logo-128.png" width="96" align="right" alt="">

Language support for [Wolang](https://github.com/MowoGames/Wolang) — a
statically typed, compiled language with gamedev-grade ergonomics.

Everything here is the compiler talking: the extension starts
`wolang lsp` and shows what it says. There is no second implementation of
the language in TypeScript, so the editor and the build never disagree.

## What it does

- **Syntax highlighting** for `.wo`, including units (`10m / 2s`),
  effects (`uses io, rand`), abilities, components and `machine`/`state`.
- **Diagnostics as you type** — types, effects, borrows, exhaustive
  `match`, unreachable states. The same messages `wolang check` prints.
- **Hover** with the type and the doc comment.
- **Go to definition**, **find references** and **document symbols**.
- **Completion** for names in scope, members, and the standard library.
- **Format on save** through `wolang fmt --stdin`.
- **Quick fixes**: the compiler suggests the change and the editor
  applies it — `W0101` offers to prefix an unused name with `_`.
- **Tasks**: `wolang check`, `test`, `build` and `run` for the package in
  the workspace.
- **Debugging** templates for LLDB, with line information from
  `wolang build`.

## Requirements

The `wolang` executable has to be on the path, or named in settings:

```jsonc
{
  "wolang.path": "/usr/local/bin/wolang"
}
```

Build it from the repository, or take a release binary:

```sh
git clone https://github.com/MowoGames/Wolang
cd Wolang
cmake --preset release && cmake --build --preset release
# the driver is at build/release/tools/wolang/wolang
```

## Settings

| Setting | What it is |
|---|---|
| `wolang.path` | The `wolang` executable. Default `wolang`. |
| `wolang.trace.server` | `off`, `messages` or `verbose`: the traffic between the editor and the server. |

## A taste of the language

```wolang
need std.io.{Print}

ability Greet {
  fun Hello() -> text
}

struct Wolf {
  var name: text
}

extend Wolf: Greet {
  fun Hello() -> text => "awoo, {name}"
}

fun main() uses io {
  const wolf = Wolf("mowo")
  Print(wolf.Hello())
}
```

## Other editors

Zed, Neovim, Sublime Text, Emacs, Helix and the JetBrains family are
supported too, all over the same `wolang lsp`. See
[editors/README.md](https://github.com/MowoGames/Wolang/blob/alpha/editors/README.md).

## Licence

Apache-2.0, the same as the language. See `LICENSE`.
