<img src="assets/logo-128.png" width="96" align="right" alt="">

# Wolang

A statically typed, compiled, general purpose language with gamedev-grade
ergonomics. Wolang is its own project: the compiler, runtime and libraries have
no dependency on any game engine, and an engine is just one of the hosts it can
be embedded into.

**Status:** the language runs, compiles to machine code, embeds in other
programs and has a standard library. Three tiers agree on every test: an
interpreter as the reference, a just-in-time tier for `run`, and an
ahead-of-time tier through LLVM for `build`. A differential fuzzer
compares all of them on generated programs, which is what finds
miscompilations before a person does.

What it has: types with inference, abilities with static and dynamic
dispatch, generics (including methods with type parameters of their own),
exhaustive `match`, optionals and `T!`, effects (`uses io`) checked
through the call graph, units and dimensions, components and systems with
a compiler-scheduled frame, `comptime`, destructors and moves, closures
that capture by copy, threads, an arena, and C interop in both
directions.

What it does not have yet: TLS, WebAssembly (the back end is there, the
toolchain is not), GPU kernels, and associated types on abilities. See
[STATUS.md](STATUS.md) for where work stopped and
[docs/decisions.md](docs/decisions.md) for why things are the way they
are.

```wolang
need std.io.{Print}

fun main() uses io {
  const name = "world"
  Print("hello, {name}")
}
```

## What makes it different

Wolang takes abilities, exhaustive `match`, `Result`, `comptime` and optionals from
the usual places. These five are the parts it is actually built around:

| | |
|---|---|
| **Effects and capabilities** | A function's signature states what it may do (`uses io, alloc, rand`). One mechanism covers determinism, allocation-free hot paths, sandboxed plugins, compile-time evaluation and server/client separation. |
| **Units in the type system** | `dimension` and `unit` declarations make `10m / 2s` a velocity and `Quat.FromAxis(up, 90deg)` reject radians. Checked at compile time, free at runtime. |
| **Layout polymorphism** | `soa []Particle` stores one array per field; `aos []Particle` stores records. Layout belongs to the storage, so the same type works both ways and `foreach` maps the loop onto SIMD lanes either way. |
| **State machines and timelines** | `machine`, `state`, `on ... after 3s ->` are language constructs, checked for unreachable states, survivable across hot reload. |
| **Reactive values** | `derived` fields recompute when their inputs change, which is what UI and stat aggregation actually need. |

Two smaller things it does differently. `T?` may be absent and `T!` may fail —
both postfix, so a type reads left to right and `Pawn?!` means either; and
optionals need no `Some`/`None` vocabulary, because `x?` and `null` already
say it:

```wolang
fun Lookup(key: text) -> Pawn?!

match count {
  n? => n
  null => 0
}
```

Memory is hybrid by design: value semantics with second-class references,
`Owned`/`Shared` for owned and shared heap data, `arena` blocks for frame
arenas, and a garbage collector that is optional — a program that never
declares a `class` does not link one.

## Building

Requires CMake 3.24+ and a C++20 compiler. No third party dependencies.

```sh
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Presets: `debug`, `asan` (address and UB sanitizers), `release`.

## Using it

```sh
wolang new game && cd game     # a package that runs, tests and builds
wolang run .                   # run it
wolang test .                  # every `@Test`, in each tier, compared
wolang build . -o game         # a binary that needs no compiler
wolang check .                 # types, effects, borrows, exhaustiveness
wolang fmt --write .           # the formatter
```

And the rest of what the driver does:

```sh
wolang repl                    # a session: type an expression, see it
wolang debug .                 # build with line information, start lldb
wolang run --profile .         # where the time goes
wolang run --record r.journal . # and --replay, to make a run happen again
wolang fetch .                 # git dependencies, written into wolang.lock
wolang doc . -o docs/api       # what a package offers, as Markdown
wolang bind sdl.h              # Wolang declarations for a C header
wolang lsp                     # what every editor in editors/ talks to
wolang fuzz --count 200        # generate programs, compare the tiers
wolang systems examples/systems # what systems touch and when they run
```

## Serving and calling

```wolang
need http.{AppNew, Ok, Request, Response, ServeWithDefaults}

fun Health(_request: Request) -> Response => Ok("ok\n")

fun main() uses io, unsafe {
  var app = AppNew()
  app.Get("/health", Health)
  const _code = ServeWithDefaults(app, 8080, 512)
}
```

`wolang-http` is Wolang over eight socket calls: a router with path
parameters and nested routers, middleware, typed extractors, static files
with the traversal checks, and a client. 53 900 requests a second on one
thread and 58 900 on eight, keep-alive, in a couple of megabytes of
resident memory.

## Embedding it

```c
#include "wolang/embed/wolang.h"

WolangVm* vm = wolang_vm_create();
wolang_vm_bind(vm, "host_log", (void*)&host_log);
wolang_vm_load(vm, "scripts/enemy");
WolangValue delta = wolang_float(1.0 / 60.0), out;
wolang_vm_call(vm, "Update", &delta, 1, &out);
```

One header, a C ABI, a static and a shared library, and
`find_package(Wolang CONFIG)` → `Wolang::embed`. See
[docs/embedding.md](docs/embedding.md).

## Layout

```
include/wolang/   public headers, mirrored by src/
  base/           arenas, interning, source management, diagnostics
  syntax/         tokens, lexer, syntax tree, parser, tree printer
  query/          the incremental query engine
  compiler/       item trees, database, manifests, modules, workspace, cache
  sema/           types, resolution, effects, dimensions, flow, systems
  ir/             typed SSA, lowering, passes, the verifier, tasks
  codegen/        the dev tier (arm64, x64) and the LLVM back end
  interp/         the reference tier
  runtime/        allocator, arena, threads, poller, world, text
  abi/            layouts: what a value is in memory
  embed/          the C ABI for embedding
  bind/           C headers in, Wolang declarations out
  lsp/            the language server
src/              implementation (.cc)
tools/wolang/     the `wolang` command line driver
lib/              std, http, game, core -- written in Wolang
editors/          VS Code, Zed, Neovim, Sublime, Emacs, Helix, JetBrains
tests/            unit tests and the dependency-free test harness
examples/         .wo sources used by hand and as compiler corpus
docs/             specification, architecture, decisions, diagnostics, style
```

## Documentation

- [docs/spec.md](docs/spec.md) — language specification
- [docs/decisions.md](docs/decisions.md) — every design decision, with the reason
- [docs/packages.md](docs/packages.md) — packages, modules and the manifest
- [docs/architecture.md](docs/architecture.md) — how the compiler is built
- [docs/roadmap.md](docs/roadmap.md) — waves and exit criteria
- [docs/diagnostics.md](docs/diagnostics.md) — diagnostic catalog
- [docs/style.md](docs/style.md) — code and commit conventions
- [docs/embedding.md](docs/embedding.md) — Wolang inside another program
- [docs/debugging.md](docs/debugging.md) — debuggers, profiles, replays
- [editors/README.md](editors/README.md) — editor support, all over `wolang lsp`
- [assets/](assets/) — the logo, the wordmark and the mascot, and the script that draws them

## Licence

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
