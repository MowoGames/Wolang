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

## Stage 0: what `std` has for a compiler

Read from `lib/std/src` at 1.2.0, not measured. "Slow" is a reading of the code,
to be confirmed by a benchmark before it is fixed.

| Need | What exists | Missing or slow |
|---|---|---|
| Fast `Map` | `std.map.Map<K, V>`: open addressing, linear probing, half load, backward-shift removal. `IntMapNew` and `TextMapNew` (1.2.0) choose the hashing. `std.hash` has FNV-style text hashing. | Slot finding is `hash % size` with a 64-bit divide on every probe start; a power-of-two table and a mask would not divide. `Get` takes a fallback rather than answering an optional, so a lookup of a class value needs a sentinel. The walk gives keys only, so values cost a second probe each. `Reserve` rehashes through `Put`, which hashes every key again; the hash is not stored. No `Entry` type. |
| Arenas | The language `arena { }` block frees what was taken inside it at block end. | No arena a compiler can hold across calls and free as one: the IR, the AST and the type store each want their own lifetime. `Builder.Release` says "nothing frees the bytes yet", so text built by a compiler is never freed outside an `arena`. A `[]T` bump allocator with `Reset` is the missing piece. |
| Text building | `std.text.Builder`: `Append`, `AppendByte`, `AppendLine` (1.2.0), `Text`, `Release`, `Clear`. `std.fmt` for numbers. | `Append` copies byte by byte; there is no bulk copy intrinsic, so a large append is a loop. `std.fmt` imports `std.text`, so `Builder` has no `AppendInt` without a cycle. No `Builder.Insert` or `Truncate`. |
| Text scanning | `Bytes`, `IndexOf`, `Substring`, `StartsWith`, `Trim`, `Split`, `ParseInt`, `ParseFloat`, `std.utf8`. | `Substring` is a view of the same bytes, not a copy, so token text is free; a lexer indexes `Bytes(source)` directly. No character classes (`IsDigit`, `IsAlpha`): each lexer writes its own. |
| Lists and sorting | `List<T>`, `Heap<T>`, `Sort`, `SortBy<T: Ord>`, `LowerBound`, `Deque`, `Set`. | `List` has `Push`, `Pop`, `At`, `Set`, `Items`; no `Insert`, `RemoveAt` or `Last`. `Sort` is a quicksort with an insertion cutoff and is not stable; a compiler that prints diagnostics in source order needs a stable sort. |
| Files | `std.fs`: `ReadAll`, `ReadText`, `WriteAll`, `WriteText`, `Append`, `Exists`, `List`, `Walk`, `Copy`, `RemoveTree`, `TempName`. `std.path`: `Join`, `Normalize`, `Parent`, `Name`, `Extension`. | `List` and `Walk` answer one `text` with separators, so a directory walk is a split. `ReadAll` takes a byte limit. No file timestamps beyond `ChangedAt`, no symlink question. |
| Process | `std.process`: `Run`, `Start`, `StartWith`, `Command` with directory and variables, exit code and captured output. `std.env`: arguments, variables. | No way to wait on several children; the linker and `cc` are run one at a time, which is what the driver does today. |
| Errors with data | `T!(A, B)` and payloads (D235). | A compiler's diagnostic is a struct, not an error: the checker collects many, so a `List<Diagnostic>` is the shape, not `T!`. |
| IR text | The printer. The reader is in `src/ir/reader.cc` (1.2.0, C++). | The Wolang side of the IR reader is stage 3. |

## Layout

The libraries stay in `lib/`. Moving them to `library/` would rename
hundreds of paths in the build, CI, the installers and the docs and give
users nothing. A Wolang compiler would live in `compiler/` beside `src/`
until stage 4, so both can be built from one checkout.
