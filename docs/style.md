# Conventions

## C++ style

The project follows the **Chromium C++ style guide**. The parts that come up
most often:

| | |
|---|---|
| File names | `snake_case`; sources end in `.cc`, headers in `.h`, x-macro includes in `.inc`, tests in `*_unittest.cc` |
| Types | `PascalCase` |
| Functions and methods | `PascalCase` |
| Variables | `snake_case` |
| Class data members | `trailing_underscore_`; plain-data struct members have no underscore |
| Constants and enum class values | `kPascalCase` |
| Namespaces | lowercase, `wolang` and `wolang::test` |
| Include guards | `WOLANG_<PATH>_H_` |
| Indentation | two spaces, 80 column limit |
| Out parameters | pointers, not non-const references |

`.clang-format` in the repository root carries `BasedOnStyle: Chromium`; run
`clang-format -i` on anything you touch.

**No exceptions and no RTTI.** `-fno-exceptions -fno-rtti` is set for every
target, which means:

- no `throw`, no `try`; report failures with status or error-code returns;
- no `dynamic_cast` and no `typeid`; use explicit tag fields;
- `std::filesystem` and `std::stoi` style throwing APIs are off limits — use
  the error-code overloads or `std::from_chars`/`strtod`.

Include order is: the matching header, C system headers, C++ standard headers,
then project headers.

## Comments

Comments explain *why*, not *what*. A header comment on a type says what the
type is for and what invariant it keeps; a comment inside a function flags
something a reader would otherwise get wrong. Restating the code is noise.

## Tests

Tests live in `tests/unit`, mirroring the source tree, and use the harness in
`tests/harness`:

```cpp
WOLANG_TEST(Lexer, LexesUnitSuffixesAsSymbols) {
  Fixture fixture("0.5s 200ms");
  WOLANG_EXPECT(fixture.Codes().empty());
}
```

- `WOLANG_EXPECT_*` records a failure and continues; `WOLANG_ASSERT_*` returns
  from the test body. There are no exceptions, so an assert cannot unwind.
- The first macro argument is the suite, which must match a suite registered in
  `tests/CMakeLists.txt` so `ctest` can address it individually.
- Diagnostics are asserted by code (`"E0001"`), never by message text, so
  wording can improve without breaking tests.
- A new diagnostic needs two tests: one where it fires and one where a similar
  but correct program does not trip it.

Run everything with `ctest --test-dir build/debug --output-on-failure`, or one
suite with `./build/debug/tests/wolang_tests --filter Lexer -v`.

## Commits

Conventional Commits, one line, lowercase subject, imperative mood, no body
unless the change genuinely needs rationale, and no trailers.

```
feat(lexer): scan unit suffixes as symbols
fix(parser): recover on a stray closing brace
docs(spec): describe effect rows
test(base): cover arena alignment
chore(cmake): add a sanitizer preset
```

Scopes are component directories: `base`, `lexer`, `parser`, `query`,
`compiler`, `format`, `sema`, `check`, `ir`, `interp`, `codegen`, `runtime`,
`std`, `cmake`, `docs`, `tests`, `tools`.

## Wave discipline

A wave is not finished until its code, its tests, `docs/`, a report in
`docs/waves/`, and `STATUS.md` all agree with each other. The point of
`STATUS.md` is that work can stop at any commit and resume without
archaeology.
