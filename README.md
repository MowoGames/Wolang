# Wolang

A statically typed, compiled, general purpose language with gamedev-grade
ergonomics. Wolang is its own project: the compiler, runtime and libraries have
no dependency on any game engine, and an engine is just one of the hosts it can
be embedded into.

**Status:** wave 2 of 16. The lexer, the parser and the diagnostic engine work:
a `.wo` file becomes a syntax tree, and broken syntax recovers instead of
giving up. There is no type checker or code generator yet. See
[STATUS.md](STATUS.md) for where work stopped and
[docs/roadmap.md](docs/roadmap.md) for what each wave adds.

## What makes it different

Wolang takes traits, exhaustive `match`, `Result`, `comptime` and optionals from
the usual places. These five are the parts it is actually built around:

| | |
|---|---|
| **Effects and capabilities** | A function's signature states what it may do (`uses io, alloc, rand`). One mechanism covers determinism, allocation-free hot paths, sandboxed plugins, compile-time evaluation and server/client separation. |
| **Units in the type system** | `dimension` and `unit` declarations make `10m / 2s` a velocity and `Quat.FromAxis(up, 90deg)` reject radians. Checked at compile time, free at runtime. |
| **Layout polymorphism** | `@layout(soa)` switches a struct between array-of-structs and struct-of-arrays without touching the code that uses it; `foreach` maps the loop onto SIMD lanes. |
| **State machines and timelines** | `machine`, `state`, `on ... after 3s ->` are language constructs, checked for unreachable states, survivable across hot reload. |
| **Reactive values** | `derived` fields recompute when their inputs change, which is what UI and stat aggregation actually need. |

Memory is hybrid by design: value semantics with second-class references,
`Box`/`Rc` for owned and shared heap data, `region` blocks for frame arenas, and
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
./build/debug/tools/wolang/wolang tokens examples/hello.wo
./build/debug/tools/wolang/wolang ast examples/turret.wo
./build/debug/tools/wolang/wolang check examples/recovery.wo
```

## Layout

```
include/wolang/   public headers, mirrored by src/
  base/           arenas, interning, source management, diagnostics
  syntax/         tokens, lexer, syntax tree, parser, tree printer
src/              implementation (.cc)
tools/wolang/     the `wolang` command line driver
tests/            unit tests and the dependency-free test harness
examples/         .wo sources used by hand and as lexer corpus
docs/             specification, architecture, roadmap, diagnostics, style
```

## Documentation

- [docs/spec.md](docs/spec.md) — language specification
- [docs/architecture.md](docs/architecture.md) — how the compiler is built
- [docs/roadmap.md](docs/roadmap.md) — waves and exit criteria
- [docs/diagnostics.md](docs/diagnostics.md) — diagnostic catalog
- [docs/style.md](docs/style.md) — code and commit conventions
