# Wolang - notes for Claude

Wolang is a statically typed, compiled language: a C++20 compiler (lexer -> parser -> sema ->
effects -> typed SSA IR -> passes) with three tiers that must agree - the interpreter (reference),
a JIT dev tier (x64/arm64) and LLVM AOT - plus libraries written in Wolang under `lib/`.
Map: `docs/architecture.md` (read by section). Where work stands: `STATUS.md` ("In short" first).

## Commands (always via the wrapper: compact output, full logs in build/logs/)
- `.claude/scripts/wl build | test [Suite] | lib [pkg] | fuzz [n] [seed] | fmt | docs | ci | wo <args>`
- `.claude/scripts/decision D73` / `decision -s <regex>` - never read docs/decisions.md whole.

## Hard rules
- Interpreter, JIT and AOT give one answer. Touching lowering, a back end, interp or runtime ->
  `wl fuzz 200` before done.
- Every fix ships the test that fails without it (`tests/unit/<layer>` `WOLANG_TEST`, or `@Test`
  beside library code). Diagnostics asserted by code, with a non-firing twin.
- No exceptions, no RTTI, no new dependencies. Comments say why and cite `(D<n>)`.
- Code, tests, docs and STATUS.md agree at every commit (`wolang-wrapup` skill).
- Commits: one-line Conventional Commits, imperative, known scope; titles are posted publicly.

## Saving tokens
- Files over 60 KB (lower.cc, type_check.cc, main.cc, emitters, decisions.md) are blocked from
  whole reads by a hook: grep first, then read 200-400 line ranges.
- Delegate: `researcher` (Haiku) for lookups, `verifier` (Haiku) to run checks, `coder` (Sonnet)
  to implement, `architect` (Opus) to design, `reviewer` (Sonnet) before commit. The main thread
  coordinates and keeps summaries, not dumps. Multi-file work: `wolang-feature` skill.
- Writing `.wo`: `wolang-lang` skill. Lone `.wo` files have no imports; use `build/scratch/` packages.

## Memory
Durable lessons are committed in `.claude/memory/`; add with the `remember` skill.
@.claude/memory/MEMORY.md
