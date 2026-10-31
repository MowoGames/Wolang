<p align="center">
  <img src="assets/logo-256.png" width="128" alt="Wolang logo">
</p>

<h1 align="center">Wolang</h1>

<p align="center">
  <a href="LICENSE">Apache-2.0</a> ·
  <a href="docs/tour.md">Tour</a> ·
  <a href="docs/spec.md">Specification</a> ·
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="https://github.com/MowoGames/Wolang/discussions">Discussions</a>
</p>

A statically typed, compiled, general purpose language with gamedev-grade
ergonomics. Wolang is its own project: the compiler, runtime and libraries have
no dependency on any game engine, and an engine is just one of the hosts it can
be embedded into.

**Status:** the language runs, compiles to machine code for five
targets, embeds in other programs, opens a window, serves HTTP and has
a standard library of thirty-five modules. Three tiers agree on every test: an
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

What it does not have yet: GPU kernels, associated types on abilities,
HTTP/2, and a garbage collector that does anything. See
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
| **State machines and timelines** | `machine`, `state`, `on ... ->` are language constructs, checked for unreachable states, survivable across hot reload. |
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

## Installing

```sh
curl -fsSL https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.sh | sh
```

PowerShell, on Windows:

```powershell
irm https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.ps1 | iex
```

Then `wolang new hello && cd hello && wolang run .`. The rest —
particular versions, installing by hand, what is in the archive — is in
[docs/install.md](docs/install.md).

## Building

Requires **clang** and CMake 3.24+. No third party dependencies; LLVM is
optional and only for `wolang build`.

```sh
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (address and UB sanitizers),
`windows` (clang-cl), `x64`. The full guide, per system, is
[docs/building.md](docs/building.md) — including why it is clang and not
whatever your machine calls `cc`.

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

fun main() uses io, net, time, unsafe {
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
- [docs/tour.md](docs/tour.md) — the language in examples, start here
- [docs/cookbook.md](docs/cookbook.md) — whole programs that do a thing
- [docs/install.md](docs/install.md) — installing it, on each system
- [docs/releasing.md](docs/releasing.md) — making a release
- [docs/building.md](docs/building.md) — building it, on each system
- [docs/stdlib.md](docs/stdlib.md) — the standard library, module by module
- [docs/style.md](docs/style.md) — code and commit conventions
- [docs/embedding.md](docs/embedding.md) — Wolang inside another program
- [docs/debugging.md](docs/debugging.md) — debuggers, profiles, replays
- [editors/README.md](editors/README.md) — editor support, all over `wolang lsp`
- [assets/](assets/) — the logo, the wordmark and the mascot, and the script that draws them

## Contributing

Wolang is open source and takes contributions: bug reports, tests,
library modules, editor support, documentation and compiler work.
[CONTRIBUTING.md](CONTRIBUTING.md) says how to build, test and send a
change, and has a list of places to start. Questions go to
[Discussions](https://github.com/MowoGames/Wolang/discussions); see
[SUPPORT.md](SUPPORT.md) for where everything else goes.

## Licence

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
