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
| Fast `Map` | `std.map.Map<K, V>`: open addressing, linear probing, half load, backward-shift removal. `IntMapNew` and `TextMapNew` (1.2.0) choose the hashing. `std.hash` has FNV-style text hashing. | Slot finding was `hash % size`, a 64-bit divide on every probe start; the table is always a power of two, so it is a mask now (D284). `Get` takes a fallback rather than answering an optional, so a lookup of a class value needs a sentinel. The walk gives keys only, so values cost a second probe each. `Reserve` rehashes through `Put`, which hashes every key again; the hash is not stored. No `Entry` type. |
| Arenas | The language `arena { }` block frees what was taken inside it at block end. | `std.arena` (D285) is the bump allocator a compiler can hold across calls: `Take`, `Reset`, drop. `Builder.Release` still says "nothing frees the bytes yet", so text built by a compiler is never freed outside an `arena`; the arena hands out bytes, not text, and a typed view of them is `ViewFromRaw`. |
| Text building | `std.text.Builder`: `Append`, `AppendByte`, `AppendLine` (1.2.0), `Text`, `Release`, `Clear`. `std.fmt` for numbers. | `Append` and `Reserve` copy with `std.mem.Copy` now (one call, D284), and `fs.ReadText` views the buffer it read instead of copying it. `std.fmt` imports `std.text`, so `Builder` has no `AppendInt` without a cycle. No `Builder.Insert` or `Truncate`. |
| Text scanning | `Bytes`, `IndexOf`, `Substring`, `StartsWith`, `Trim`, `Split`, `ParseInt`, `ParseFloat`, `std.utf8`. | `Substring` is a view of the same bytes, not a copy, so token text is free; a lexer indexes `Bytes(source)` directly. No character classes (`IsDigit`, `IsAlpha`): each lexer writes its own. |
| Lists and sorting | `List<T>`, `Heap<T>`, `Sort`, `SortBy<T: Ord>`, `LowerBound`, `Deque`, `Set`. | `List` has `Push`, `Pop`, `At`, `Set`, `Items`; no `Insert`, `RemoveAt` or `Last`. `Sort` is a quicksort with an insertion cutoff and is not stable; a compiler that prints diagnostics in source order needs a stable sort. |
| Files | `std.fs`: `ReadAll`, `ReadText`, `WriteAll`, `WriteText`, `Append`, `Exists`, `List`, `Walk`, `Copy`, `RemoveTree`, `TempName`. `std.path`: `Join`, `Normalize`, `Parent`, `Name`, `Extension`. | `List` and `Walk` answer one `text` with separators, so a directory walk is a split. `ReadAll` takes a byte limit. No file timestamps beyond `ChangedAt`, no symlink question. |
| Process | `std.process`: `Run`, `Start`, `StartWith`, `Command` with directory and variables, exit code and captured output. `std.env`: arguments, variables. | No way to wait on several children; the linker and `cc` are run one at a time, which is what the driver does today. |
| Errors with data | `T!(A, B)` and payloads (D235). | A compiler's diagnostic is a struct, not an error: the checker collects many, so a `List<Diagnostic>` is the shape, not `T!`. |
| IR text | The printer. The reader is in `src/ir/reader.cc` (1.2.0, C++). | The Wolang side of the IR reader is stage 3. |

## The throwaway lexer

`tools/selfhost/lexer` is a Wolang lexer for Wolang (package `wolexer`, tests with
`wl wo test tools/selfhost/lexer`): identifiers, keywords and reserved words,
numbers with suffixes and ranges, strings with interpolation, raw strings,
character literals, nested block comments, doc comments, `starts_line` and line
numbers, and the longest operator. It is not the stage 2 lexer: it does not
report errors, does not decode literals and does not check Unicode. It was written
to find gaps, and it was written without being run. The gaps below are what
writing it ran into; the first run will add more.

1. **`unsafe` everywhere.** Reading the bytes of a `text` is `Bytes(...)`, which
   is `uses unsafe`, so every function of a lexer, and then of a parser, says
   `uses unsafe`. The effect stops saying anything. The read-only slice (D233)
   is what would let `Bytes` be safe.
2. **No byte literal.** `'a'` is a `char`, not a `u8`, so a byte test is written
   with a number (`byte == 123`). A `b'{'` spelling, or a comparison of a `u8`
   with a `char` literal, would make the lexer readable.
3. **Braces in text.** A `{` in a string starts an interpolation, so a program
   that talks about braces writes `\{` and `\}` (the operator table does). A
   raw string avoids it but cannot hold a `"`. A compiler prints C-like code and
   dumps, so it will meet this daily.
4. **Text building.** `Text + text` gives an owned `Text`, which does not assign
   to a `text` variable; building output goes through `Builder`. That is fine,
   but it is the first thing a newcomer trips on.
5. **No list or array literal.** The keyword table and the operator table are
   split from one string and pushed. A `[ "a", "b" ]` literal for a `List<text>`
   would remove that.
6. **Unicode.** `std.utf8` has width and count but no identifier classes (XID) and
   no normalization (NFC) checks, which D26 requires. The lexer takes every byte
   from 128 up as part of an identifier.
7. **Numbers.** `ParseInt` reads decimal `int` only. A literal is arbitrary
   precision until it meets a type (D9), and the lexer would need a big integer
   for that; `std` has none.
8. **Two results.** `ScanNumber` returns a `Scan` struct because it wants an end
   and a flag. Tuples exist and were not tried; a note for the first run.
9. **Tables.** `Map<text, int>` answers a keyword lookup. `Get` with a fallback
   was enough here; a parser's symbol tables will want an optional-returning
   lookup.

## Stage 1 measurements

`tools/selfhost/bench` (`wolang run tools/selfhost/bench`) times what a compiler
leans on. Windows x86-64, dev tier, while seven other builds ran on the same
machine, so read the figures as orders of magnitude; the first column is before
D284 and D285, the second after.

| What | Before | After |
|---|---|---|
| `Map<int, int>`, 300,000 puts and gets | 73 ms | 56-85 ms |
| `Map<text, int>`, 100,000 distinct keys | 66 ms | 67-78 ms |
| `Builder`, 2,000,000 small appends | 130 ms | 60-65 ms |
| `fs.ReadText` of a 4.4 MB file | 95 ms | 6-7 ms |
| `ArrayNew` and `Free`, 500,000 blocks of 512 bytes | 281 ms | 273-293 ms |
| `Arena.Take` of the same blocks, reset every thousand | n/a | 8-10 ms |

- The mask in `Map.Slot` did not show above the noise; the divide it removes is
  a few nanoseconds per lookup. The text map is dominated by hashing a `text`,
  which is a byte loop; a hash that reads a word at a time is the next thing.
- `ReadText` was slow because `ReadAll` allocated and zeroed the whole `limit`
  (128 MB in this run) every time, and then copied byte by byte. It sizes the
  buffer to the file and views it now.
- `ArrayNew` and `Free` cost about half a microsecond a pair, which the arena
  takes to about twenty nanoseconds.

## The first tool: fetch and lock

`tools/selfhost/fetch` (package `wofetch`) is `wolang fetch` in Wolang (D286):

    wolang run tools/selfhost/fetch -- fetch <package directory> [--cache <directory>]

It reads the manifests with `std.toml`, walks the dependency graph the way the
workspace loader does (path dependencies by their directory, git dependencies by
a checkout named after the repository and the revision), clones with the `git`
on the path, and writes `wolang.lock`. It needs no parser.

`tools/selfhost/diff_fetch.py` is the check: it builds local git repositories (one
depending on another), makes two copies of an application that reaches them through
a path dependency and through git, runs the C++ tool in one and the Wolang tool in the
other, and compares the two lock files byte for byte. It does, on four locked packages.

What the tool leaves out: it does not check version requirements (the build does),
and it does not report a manifest key it does not know.

Writing it found a bug in the C++ tool: the clone was run through `std::system`
with single quotes and `2>/dev/null`, which `cmd.exe` does not understand, so no
fetch worked on Windows. The C++ tool quotes and discards errors per platform now,
and `Workspace.FetchesAGitDependencyAndLocksIt` fetches from a local repository
(and skips when `git` is missing). The Wolang tool never had the problem: `std.process.Start`
runs the program directly, with no shell.

## Gaps stage 1 found

1. **The dev tier does not compare two enum values.** `a == Kind.File` is "compares
   two aggregates", and a function the dev tier refuses takes every caller into
   the interpreter, which has a step limit. `std.fs` reads the tag with `match`
   now; the emitters should compare the tag word of a payload-free enum.
2. **No `u64` to float conversion in the dev tier.** The lexer converts through
   two `u32` halves (`U64ToFloat`). The same refusal hides in any code that
   turns a 64-bit count into a float.
3. **`after`, `before` and `arena` are keywords everywhere**, so they cannot be
   variable names; a lexer wants to call a variable `after`. A contextual
   keyword would cost nothing here (they are only keywords in a system's
   header and a block).
4. **An array literal exists** (`[false, false]`), unlike what the throwaway
   lexer's gap list said; what is missing is a `List` literal.
5. **`Text` of a built string cannot be freed**, so a compiler's memory grows
   with every text it makes; the arena holds bytes and does not fix that.

## Layout

The libraries stay in `lib/`. Moving them to `library/` would rename
hundreds of paths in the build, CI, the installers and the docs and give
users nothing. A Wolang compiler would live in `compiler/` beside `src/`
until stage 4, so both can be built from one checkout.
