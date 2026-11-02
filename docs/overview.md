# Wolang

Wolang is a statically typed, compiled language for games and the
services around them. It is its own project — the compiler, runtime and
libraries depend on no engine — and Mowo Engine is one of the hosts it
embeds into.

```wolang
need std.io.{Print}

dimension Length
dimension Time
dimension Velocity = Length / Time

unit m: Length
unit s: Time

class Turret {
  var range: float = 12.0
  weak var target: Enemy? = null
}

class Enemy {
  var health: int = 100
}

fun main() uses io {
  const speed: Velocity = 3.0m / 1.5s
  const turret = Turret(12.0, null)
  Print("speed {speed}, has a target: {turret.target != null}")
}
```

## What it is like

- **Types with inference.** Write the type where it helps the reader and
  leave it out where it does not. No implicit numeric conversions: `as`
  is the one way to convert, and a narrowing one traps instead of
  wrapping.
- **Effects.** `uses io` says that a function prints; the checker follows
  the call graph and refuses a caller that does not admit what its
  callees do. A package can be granted no capabilities at all.
- **Units and dimensions.** `3.0m / 1.5s` is a `Velocity`; adding metres
  to seconds is a compile error.
- **Memory without a pause.** A `struct` is a value. A `class` is
  counted: the last reference frees it, with everything it holds, and
  `weak var` breaks a cycle. An `arena { }` block frees everything it
  allocated at once, which is what a frame loop wants.
- **Gameplay in the language.** Components and systems with an access
  graph the compiler schedules across threads, `soa` and `aos` layouts
  behind one syntax, state machines, signals and `derived` fields.
- **Three tiers that agree.** An interpreter is the reference; `wolang
  run` compiles to machine code on the fly for x86-64 and arm64; `wolang
  build` produces a native binary through LLVM. A differential fuzzer
  runs the same generated programs through all of them in CI on every push.
- **C in both directions.** Call C with `extern "C"`, and embed Wolang in
  a C or C++ program through one header.

## Where to start

1. [Install](/docs/wolang/install/) — one line on macOS and Linux, an
   `.msi` on Windows.
2. [A tour](/docs/wolang/tour/) — the whole language in half an hour,
   every example compiled on each commit.
3. [Cookbook](/docs/wolang/cookbook/) — short answers to "how do I…".
4. [Standard library](/docs/wolang/stdlib/) — thirty-five modules, from
   text and collections to HTTP, TLS, compression and time zones.
5. [Language reference](/docs/wolang/spec/) — the grammar and the rules,
   for when the tour is not precise enough.

## Platforms

| | |
|---|---|
| Systems | Windows x86-64, Linux x86-64 and arm64, macOS arm64 and x86-64 |
| Tiers | interpreter (reference), machine code for `run`, LLVM for `build` |
| Cross targets | `--target` for Windows, Linux, WebAssembly, RISC-V |
| Editors | VS Code, JetBrains, Neovim, Emacs, Sublime, Helix, Zed through one language server |

Wolang is open source under the Apache-2.0 licence:
[github.com/MowoGames/Wolang](https://github.com/MowoGames/Wolang).
