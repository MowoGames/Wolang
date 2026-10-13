# Wave 1 — project foundation and lexer

**Status:** complete. 104 unit tests, all passing.

## Scope

Stand the project up as an engine-independent CMake project, build the support
layer every later wave depends on, and implement the lexer in full.

## What was built

### Build system

- Top-level CMake project with `WOLANG_BUILD_TESTS`, `WOLANG_BUILD_TOOLS`,
  `WOLANG_WARNINGS_AS_ERRORS` and `WOLANG_ENABLE_SANITIZERS` options.
- `CMakePresets.json` with `debug`, `asan` and `release` presets.
- `cmake/WolangCompilerOptions.cmake` applies the warning set and disables
  exceptions and RTTI for every target, as Chromium style requires.
- No third party dependencies, including for tests.

### Base layer — `include/wolang/base`, `src/base`

| Component | Notes |
|---|---|
| `Arena` | Bump allocator, geometric chunk growth capped at 4 MB, `New`, `NewArray`, `CopyString`, `Reset` that keeps one chunk. No destructors are ever run, enforced with a `static_assert`. |
| `StringInterner`, `Symbol` | `Symbol` is a `uint32`, so name comparison is one integer compare. Text is copied into an arena, so interning outlives the caller's buffer. Hashable for use as a map key. |
| `SourceManager`, `SourceSpan` | Owns file text, computes line tables eagerly at registration so position queries need no mutable state. Handles `\r\n`, clamps out-of-range offsets, and `LoadFile` reports failures through an out parameter rather than throwing. |
| `DiagnosticEngine`, `DiagnosticBuilder` | Chained construction (`Primary`, `Secondary`, `Note`, `Suggest`) that emits on destruction, per-code severity overrides including `kOff`, and counting by severity. |
| `RenderDiagnostic` | rustc-style output: header, location, source lines, `^` and `-` markers, `...` for spans that continue, notes, and fixes rendered with the edit applied. Expands tabs and treats multi-byte characters as one column so markers line up. |

### Syntax layer — `include/wolang/syntax`, `src/syntax`

- `token_kind.inc` is the single source of truth for the token vocabulary:
  specials, 51 punctuation tokens, 48 keywords, 9 contextual keywords and 40
  reserved words, consumed through x-macros by everything that needs a table.
- `Token` is 16 bytes with payloads in side tables (`numbers`, `strings`,
  `comments`), enforced by `static_assert`.
- `Lex()` handles: BOM, shebang, line and documentation comments, identifiers
  including non-ASCII, all four integer bases with digit separators, floats
  with exponents, type suffixes, unit suffixes, strings with escapes and
  interpolation, raw strings, character literals, and every operator with
  longest-match rules.
- Eleven lexical diagnostics (`E0001`–`E0011`), each with recovery so one bad
  token does not hide the rest of the file.

### Driver — `tools/wolang`

`wolang check`, `wolang tokens`, `wolang version`, with `--no-color` and
`--trivia`. Colors are only emitted when stderr is a terminal.

### Tests — `tests`

A dependency-free harness (`WOLANG_TEST`, `WOLANG_EXPECT_*`, `WOLANG_ASSERT_*`)
that works without exceptions: asserts return from the test body. Suites are
registered individually with `ctest`, so a failure names the component.

104 tests: arena alignment and growth, interning identity and stability,
position mapping and line handling, diagnostic configuration, three golden
renderer outputs, token tables, and the lexer across structure, identifiers,
operators, numbers, strings, characters, trivia and byte-level edge cases.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| Wolang is a standalone project, not an engine module | the language is meant for general use; an engine becomes one host among several |
| Hybrid memory model, optional garbage collector | value semantics and regions for predictable code, a collector only for object graphs that need one, so WASM and embedded targets stay viable |
| Effects inferred inside a module, explicit on `pub` signatures | one mechanism instead of five unrelated attribute checks |
| Units, layout polymorphism, state machines and reactive values are core features | each is a repeated source of gameplay bugs or rewrites, and none can be retrofitted as a library |
| Declaration words such as `state` and `unit` are contextual keywords | gameplay code uses them as variable names constantly |
| Unit suffixes are recorded as symbols, not matched against a built-in list | units are user declared, so only sema can resolve them |
| No block comments | `//` and `///` are enough, and nesting rules for `/* */` are a recurring source of lexer bugs |
| arm64 before x64 for the dev tier | that is the development machine |
| The standard library is written in Wolang above a C++ platform floor | see `docs/roadmap.md`; `core` becomes possible in wave 7 |
| The compiler stays C++20, self-hosting is not a goal | self-hosting requires the finished language and doubles the work |

## Things worth remembering

- `"??="` is a C++ trigraph for `#`; the token table writes it `"?\?="`.
- Byte values above 0x7F cannot appear in narrow string literals under
  `-Werror`, so the BOM and invalid-UTF-8 tests build their input with
  `push_back(static_cast<char>(...))`.
- The lexer deliberately does not own a `SourceManager` reference; it takes the
  text once, which `-Wunused-private-field` caught immediately.

## Deferred to later waves

- Unicode XID identifier classes (currently any well-formed code point).
- An interpolation that contains a string containing another interpolation is
  skipped by brace counting rather than recursive lexing.
- Line and column numbers are byte based; the language server will need UTF-16
  conversion in wave 14.
- Column information for multi-line spans is simplified to the first line.

## Next

Wave 2: the AST and the parser. See `STATUS.md`.
