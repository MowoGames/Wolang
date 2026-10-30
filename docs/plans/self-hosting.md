# Self-hosting: a plan, not a commitment

The roadmap says self-hosting is not a goal before phase D is done. This
is what it would take after that, written down so the decision can be made
with the cost in view. Nothing here is scheduled.

## Why it might be worth it

- The compiler is the largest program anyone will write in Wolang. Writing
  it in Wolang finds what the language and `std` lack faster than any
  benchmark.
- Contributors could change the language in the language.
- The embedding story stays: a compiler built by `wolang build --library`
  is a C library like any other.

## Why not now

- The language is not frozen. The 1.2.0 decisions change errors (D235),
  `unsafe` (D236), `int` (D238), abilities (D239) and closures (D240). A
  compiler written before them is written twice.
- The C++ compiler is about 56,000 lines: three tiers, two hand-written
  emitters, the LLVM-C bindings, the LSP, the debugger and the embedding
  API. A rewrite in one go would stop everything else for a year.
- Something has to build the first Wolang compiler. That is the C++ one,
  which therefore stays.

## What stays C++

The runtime (`src/runtime`) and the platform layer (`platform/`): they
are the floor the language stands on, the syscalls, Win32, Cocoa and X11.
The LLVM back end talks to LLVM through its C API from whichever language
the compiler is in.

## Stages

Each stage is checked the way the tiers are: the new part and the old part
are given the same input and must give the same output.

| Stage | What | Checked by | Needs |
|---|---|---|---|
| 0 | The 1.2.0 language decisions land; `std` has what a compiler needs: a fast `Map`, arenas, text building, files, process | the existing suites | 1.2.0 |
| 1 | Tools in Wolang: `fmt`, the doc generator, the package manager's fetch and lock | byte-identical output against the C++ tools on `lib/`, `examples/` and the docs | stage 0 |
| 2 | Lexer and parser in Wolang | the same tokens and the same AST dump on every file in the repository and every fuzz program | stage 1 |
| 3 | Sema and lowering in Wolang, producing the same IR | IR dumps compared; this needs the IR reader that is on the "not done" list | stage 2 |
| 4 | The driver built by the Wolang compiler through AOT; the C++ compiler frozen as stage 0, still built in CI and used to bootstrap | the full suite and the fuzzer run on both compilers | stage 3 |

The interpreter, the JIT emitters and the LSP move last, if ever: they are
where the C++ compiler is fastest and where a rewrite buys the least.

## Layout

The libraries stay in `lib/`. Moving them to `library/` would rename
hundreds of paths in the build, CI, the installers and the docs and give
users nothing. A Wolang compiler would live in `compiler/` beside `src/`
until stage 4, so both can be built from one checkout.
