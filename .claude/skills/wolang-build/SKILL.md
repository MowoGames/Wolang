---
name: wolang-build
description: Build, test, fuzz and check the Wolang repo with compact output. Use whenever you need to compile, run unit/library tests, run the tier fuzzer, or reproduce CI.
allowed-tools: Bash(.claude/scripts/wl:*)
---

# Build and verify

Always go through `.claude/scripts/wl`; it writes full logs to `build/logs/` and prints a
verdict plus only the failing lines. Raw `cmake`/`ctest` output costs thousands of tokens.

| Need | Command |
|---|---|
| build (configures with clang if needed) | `wl build` / `wl build asan` |
| all unit tests (~716) | `wl test` |
| one suite (`Lexer`, `Parser`, `TypeCheck`, `Lower`, `Interpreter`, ...) | `wl test <Suite>` |
| library @Tests | `wl lib` or `wl lib std http` |
| tiers vs each other | `wl fuzz 200` (`wl fuzz 50 <seed>` to replay a seed) |
| Wolang formatting | `wl fmt` |
| include / doc snippet checks | `wl docs` |
| everything CI does on Linux | `wl ci` |
| the driver | `wl wo <args>` |
| more of a log | `wl log build 120`, `wl log test`, `wl log fuzz` |

Facts:
- Configure refuses non-clang (D184); `wl` exports `CC=clang CXX=clang++`.
- Clean debug build ~40 s on 4 cores; incremental is seconds. Presets: debug, asan, release, x64.
- Suite names are the first `WOLANG_TEST` argument; list with `build/debug/tests/wolang_tests --list | head`.
- Which checks to run for what changed:
  - syntax/sema only -> `wl test <Suite>`, then `wl test`
  - ir/codegen/interp/runtime -> also `wl fuzz 200` (several seeds if it touches lowering)
  - lib/* -> `wl lib <pkg>` and `wl fmt`
  - docs with code blocks -> `wl docs`
- For long runs whose output you don't need, hand the job to the `verifier` agent.
