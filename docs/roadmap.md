# Roadmap

Work is cut into waves. Each wave ends with a verifiable exit criterion, and
each wave closes with the same ritual:

1. code and tests land together — no wave is done with untested code;
2. `docs/` is updated for whatever the wave decided or changed;
3. a wave report is written to `docs/waves/wave-NN.md`;
4. `STATUS.md` records where work stopped and what is next.

Dates are not fixed. Scope is.

## Phase A — front end

| Wave | Content | Exit criterion |
|---|---|---|
| **1** | CMake project, test harness, base layer (arenas, interning, source manager, diagnostics with a rustc-style renderer), **lexer** | `wolang tokens` and `wolang check` work; lexer covered by tests ✅ |
| **2** | AST in flat arrays, recursive-descent parser with a Pratt expression parser, error recovery, s-expression tree printer | every construct in `docs/spec.md` parses; golden AST tests ✅ |
| **3** | Query database (red-green incremental), module and package graph, `wolang.toml`, on-disk cache, `wolang fmt` | editing a function body recomputes that function only ✅ |
| **4** | Grammar completion (`extend`, `T!`, `[]T`, `soa`, labels, Unicode identifiers) ✅, then types: generics, abilities, nullability, flow typing, **units and dimensions** | the examples type check; unit mismatches are errors ✅ |
| **5** | **Effects and capabilities**, checker v1, inheritance, second-class references, `@allow` | diagnostic catalog v1, each lint with a "fires" and a "does not fire" test ✅ |
| **6** | Typed SSA IR, lowering, verifier, mid-level optimizations | IR dump stable, verifier green on every test input ✅ |

## Phase B — execution

| Wave | Content | Exit criterion |
|---|---|---|
| **7** | IR interpreter (the reference semantics), `comptime` evaluation, `wolang run`, first `core` modules written in Wolang | `examples/hello.wo` runs; `@Test` functions execute ✅ |
| **8** | Dev tier: copy-and-patch code generation (arm64 first, then x64), faults, unwind tables, symbolization | gameplay microbenchmarks within 3x of C++ ✅ (three of four against `-O2`; all four against scalar C++). x64 landed in wave 10 ✅ |
| **9** | Memory: value semantics, second-class references, `Owned`/`Shared`, `arena` blocks, optional GC; C ABI FFI | a program with no `class` links no collector — vacuously, since nothing allocates through a collector yet. The FFI, raw pointers, `defer` and `E0121`–`E0123` landed; owning types need destructors ⏳ |

## Phase C — the gamedev core

| Wave | Content | Exit criterion |
|---|---|---|
| **10** | `task` with structured concurrency, signals, `derived`, **state machines**, a minimal HTTP/1.1 server as the design's driver | 100k concurrently waiting coroutines ✅ (48.7 ms for 500k resume steps, 2 MB of frames); the server serves keep-alive traffic ✅ (30k req/s, which is the client's ceiling); signals, `derived` and machines ✅ |
| **11** | ECS, access graph, parallel scheduler, **AoS/SoA layout polymorphism**, SPMD `foreach` | 1M entity integration benchmark ✅ (since D105: 25.1 ms SoA against 21.1 AoS on the four-field pass, and 22.3 against 23.9 on the one-field one — the layout wins where it should and loses where it should); `soa []T`, the optimizer that makes it pay, generics across modules, an ECS library in Wolang, and `system` with the access graph ✅. A world the systems run over, `Query` built from it, `foreach` over a dense column and stages in threads ✅ (D100, D101) — 100k entities × 10 frames in 12 ms. SPMD `foreach` (one body, several lanes) is carried forward ⏳ |
| **12** | LLVM AOT, WASM, differential fuzzing across the tiers | `wolang build` produces a binary that needs no compiler ✅ (D98); the fuzzer compares four tiers with `--aot` ✅; threads, a lock and an atomic in the runtime ✅ (D97). WASM is carried forward ⏳ |
| **12** | LLVM AOT, WASM target, differential fuzzing across interpreter, JIT and AOT | shipping builds need no JIT; within 1.2x of C++ |

## Phase D — ecosystem and hosts

| Wave | Content | Exit criterion |
|---|---|---|
| **13** | `std`, `math`, `game` libraries; readiness notification (kqueue, epoll, io_uring); `wolang-http` as a real library; package manager; documentation generator | a third party package is consumable in one line |
| **14** | LSP, debug adapter, profiler, REPL, editor integration | completion, go-to-definition and breakpoints work |
| **15** | Embedding C API; a MowoEngine host adapter; graph/text bridge | gameplay written in Wolang runs inside the engine |
| **16** | Transactional and rollback blocks, determinism and replay, mod sandboxing, GPU kernels | rollback netcode on a vertical slice |

`wolang-http` is deliberately spread across three waves rather than placed at
the end: a minimal server at wave 10 while the concurrency model is still
soft, a real one at wave 13 once `std` has sockets, and benchmarked at wave 12
when the AOT tier makes a number mean something. See decision D40.

Game servers are a different workload and are not this: they want UDP,
deterministic ticks and rollback, which is wave 16. HTTP is for the
infrastructure around a game — matchmaking, accounts, telemetry — and for
services generally.

The critical path to "the language is alive" is waves 1–7. The interpreter
arrives before the JIT on purpose: it is the reference semantics every later
tier is checked against, and it makes the standard library possible.

## Platform order

The dev machine is arm64 macOS, so the dev-tier code generator targeted
**arm64 first**, then x86-64 — both are in, and the second one is tested on
the first by building the compiler for x86-64 and letting the system's
translator run it (`cmake --preset x64`). The AOT tier is portable from the start because LLVM handles
the back end. Consoles, when they come, are AOT only.

## How the standard library is layered

The library is written in Wolang, with a thin non-Wolang floor:

| Layer | Language | Notes |
|---|---|---|
| `core` | Wolang | No allocation, no effects: `Option`, `Result`, slices, iterators, math, formatting abilities. Usable on WASM and embedded targets. |
| `std` | Wolang | Collections, strings, time, filesystem, process, threads, networking. Calls the platform layer through `extern "C"`. |
| `math`, `game` | Wolang | Vectors and matrices, ECS, state machines, tweening, input and asset abstractions — host agnostic. |
| intrinsics | compiler | Things the language cannot express: `memcpy`, atomics, SIMD operations, `sqrt`/`fma`, memory barriers. Declared as `@intrinsic fn` and lowered straight to IR operations, with no FFI cost. |
| platform | C++ | Syscalls, memory pages, threads, sockets. The only part that is not Wolang. |
| host adapters | C++ | Engine, desktop, web. Optional. |

`core` cannot exist before wave 7, because until the interpreter lands there is
nothing to execute Wolang with. That is why wave 7 explicitly includes the first
`core` modules.

**Self-hosting is not a goal.** The compiler stays C++20. Rewriting it in Wolang
would require the whole language to be finished first and would double the work
for years; it stays an option after phase D, not a commitment.
