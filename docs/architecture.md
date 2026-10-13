# Architecture

## Three layers

```
+-- Language core ------------------------------------------------+
| lexer . parser . modules . types . effects . IR . comptime      |
| interpreter . dev JIT . LLVM AOT . WASM                         |
| Knows nothing about any engine. The collector is optional.      |
+-----------------------------+-----------------------------------+
+-- Libraries ----------------+-----------------------------------+
| core (no alloc) . std . math . game (ECS, FSM, time)            |
+-----------------------------+-----------------------------------+
+-- Hosts --------------------+-----------------------------------+
| game engine . desktop . web/wasm . dedicated server             |
+-----------------------------------------------------------------+
```

Everything above the core is replaceable. The core never grows a dependency on
a host; a host reaches the language through the embedding API and through
`extern "C"` declarations.

## Compiler pipeline

```
 .wo source
   |
   v  [1] lexer       hand written, interns identifiers, records line starts
   |                  payloads in side tables so a token stays 16 bytes
   v  [2] parser      recursive descent with a Pratt expression parser,
   |                  error recovery by synchronizing on `}` and keywords
   v  [3] AST         data oriented: flat arrays, 32-bit indices, arenas
   v  [4] sema        name resolution, bidirectional inference, flow typing,
   |                  dimensional analysis for units
   v  [5] effects     inferred per function, checked against declarations
   v  [6] checker     bug, performance and style diagnostics with quick fixes
   v  [7] IR          typed SSA, block parameters, explicit safety operations
   v  [8] mid-opt     inlining, constant folding, check elimination,
   |                  devirtualization, loop vectorization
   +--> [9a] interpreter   reference semantics, comptime, bootstrap
   +--> [9b] dev tier      copy-and-patch to machine code, patch slots
   +--> [9c] ship tier     LLVM IR, -O2, ThinLTO, object files
   +--> [9d] wasm          for the web and for sandboxed plugins
```

One IR means one semantics. The interpreter is the specification of behavior;
the JIT, the AOT tier and the WASM back end are mechanical lowerings of the
same verified IR, and differential fuzzing across all four is what keeps them
honest.

## Incremental compilation

Every fact the compiler knows is a *query* with a cached result and a
dependency list: `Tokens(file)`, `ItemTree(file)`, `Signature(fn)`,
`Layout(type)`, `TypeCheck(fn)`, `Diagnostics(fn)`, `LowerIr(fn)`,
`DevCode(fn)`.

Invalidation is red-green: when an input changes, dependents are marked
possibly-dirty; a re-query recomputes and, if the new result *equals* the old
one, dependents stay green. Changing a function body therefore recompiles that
body and nothing else, because its signature did not change.

The same query engine backs the compiler, the language server and the editor,
so there is exactly one implementation of every rule.

## Core data structures

| Type | Shape | Why |
|---|---|---|
| `Symbol` | `uint32` index into a `StringInterner` | name comparison is one integer compare |
| `FileId`, `NodeId`, `TypeId`, `FuncId`, `BlockId`, `ValueId` | `uint32` indices into flat arrays | compact, cache friendly, serializable |
| `SourceSpan` | `{FileId, uint32 begin, uint32 end}` | 12 bytes; line and column are derived on demand |
| `Token` | 16 bytes, payloads in side tables | a file of tokens stays in cache |
| `Arena` | bump allocator, no destructors | per-file and per-function lifetimes are free to drop |

Types are interned, so type equality is a `TypeId` comparison.

## Memory model

Decided in wave 0, implemented in wave 9:

- `struct` is a value type with mutable value semantics; copies are copies.
- References are **second class**: they may be parameters and locals but not
  fields, which gives memory safety without a borrow checker.
- `Box<T>` is unique ownership with compile-time move checking.
- `Rc<T>` and `Arc<T>` are shared ownership.
- `region { ... }` is an arena whose values may not escape it, checked by
  escape analysis.
- `class` is a garbage collected reference type, and the collector is a
  separate optional component. A program that declares no `class` links no
  collector, which is what makes WASM and embedded targets practical.

## Effects and capabilities

A function's effects are inferred inside a module and written explicitly on
public signatures:

```wolang
pub fn Integrate(dt: Seconds, q: Query<mut Transform, Velocity>)
pub fn LoadLevel(path: str) -> Result<Level> uses io, alloc
```

Built-in effects are a closed set (`alloc`, `io`, `rand`, `time`, `net`, `gpu`,
`unsafe`, `panic`); user code declares additional capability tokens with
`effect`. This one mechanism replaces what would otherwise be five unrelated
checks: determinism, allocation-free hot paths, mod sandboxing, `comptime`
purity and server/client separation.

## Interoperability

The C ABI is the primary boundary: `extern "C"` declarations call native code
directly, with no marshalling. C++ and engine reflection are handled by an
adapter that generates Wolang declarations from the host's metadata, which
keeps that complexity out of the core.

## Directory map

| Directory | Contents |
|---|---|
| `include/wolang/base`, `src/base` | arenas, interning, source management, diagnostics and rendering |
| `include/wolang/syntax`, `src/syntax` | token vocabulary, lexer; parser and AST from wave 2 |
| `tools/wolang` | the command line driver |
| `tests/harness` | the dependency-free test harness |
| `tests/unit` | unit tests mirroring the source tree |
| `examples` | `.wo` sources, also used as compiler corpus |

Planned directories, one per wave that introduces them: `sema`, `check`, `ir`,
`opt`, `interp`, `codegen`, `runtime`, `object`, `interop`, `ecs`, `net`.
