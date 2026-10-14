# Wolang

A statically typed, compiled, general purpose language with gamedev-grade
ergonomics. Wolang is its own project: the compiler, runtime and libraries have
no dependency on any game engine, and an engine is just one of the hosts it can
be embedded into.

**Status:** wave 3 of 16. The front end is incremental: a package of `.wo`
files becomes syntax trees and module scopes, editing a function body
recomputes only that function, and there is a formatter and an on-disk cache.
There is no type checker or code generator yet. See [STATUS.md](STATUS.md) for
where work stopped and [docs/roadmap.md](docs/roadmap.md) for what each wave
adds.

## What makes it different

Wolang takes traits, exhaustive `match`, `Result`, `comptime` and optionals from
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
`Owned`/`Shared` for owned and shared heap data, `arena` blocks for frame arenas, and
a garbage collector that is optional — a program that never declares a `class`
does not link one.

## Building

Requires CMake 3.24+ and a C++20 compiler. No third party dependencies.

```sh
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Presets: `debug`, `asan` (address and UB sanitizers), `release`.

## Trying the current wave

```sh
W=./build/debug/tools/wolang/wolang

$W modules examples/package    # the package and module graph
$W check examples/package      # check a whole package
$W fmt examples/turret.wo      # format a file
$W ast examples/turret.wo      # print the syntax tree
$W check examples/recovery.wo  # see error recovery
```

## Layout

```
include/wolang/   public headers, mirrored by src/
  base/           arenas, interning, source management, diagnostics
  syntax/         tokens, lexer, syntax tree, parser, tree printer
  query/          the incremental query engine
  compiler/       item trees, database, manifests, modules, workspace, cache
  format/         the formatter
src/              implementation (.cc)
tools/wolang/     the `wolang` command line driver
tests/            unit tests and the dependency-free test harness
examples/         .wo sources used by hand and as compiler corpus
docs/             specification, architecture, roadmap, diagnostics, style
```

## Documentation

- [docs/spec.md](docs/spec.md) — language specification
- [docs/decisions.md](docs/decisions.md) — every design decision, with the reason
- [docs/packages.md](docs/packages.md) — packages, modules and the manifest
- [docs/architecture.md](docs/architecture.md) — how the compiler is built
- [docs/roadmap.md](docs/roadmap.md) — waves and exit criteria
- [docs/diagnostics.md](docs/diagnostics.md) — diagnostic catalog
- [docs/style.md](docs/style.md) — code and commit conventions
