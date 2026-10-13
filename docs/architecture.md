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
dependency list. Queries come in two kinds: **inputs**, supplied from outside
and the only thing that can start an invalidation, and **derived** values,
computed from other queries.

| Query | Kind | Value |
|---|---|---|
| `SourceText(file)` | input | a fingerprint of the file's text |
| `ModuleFiles(module)` | input | the files of a module |
| `Workspace` | input | the set of packages and modules |
| `Tokens(file)` | derived | the token stream |
| `ParsedFile(file)` | derived | the syntax tree |
| `ItemTree(file)` | derived | the declarations with signatures but no bodies |
| `ModuleScope(module)` | derived | the names a module declares |
| `Imports(file)` | derived | the resolved imports |
| `FileDiagnostics(file)` | derived | every diagnostic for the file |

Later waves add `TypeCheck(fn)`, `Effects(fn)`, `LowerIr(fn)` and `DevCode(fn)`
to the same engine.

Dependencies are **discovered, not declared**: every `Get()` a computation
makes is recorded, so a computation and its dependency list can never disagree.

Invalidation is red-green. Changing an input bumps a global revision and
invalidates nothing eagerly. The next `Get()` walks the dependency graph,
verifies each dependency, and recomputes only what actually moved. The payoff
comes from **back-dating**: a query that is recomputed but produces a value
*equal* to its predecessor keeps its old `changed_at` stamp, so everything
downstream stays green.

That is what makes `ItemTree` the pivot of the whole design. It holds each
declaration's kind, name, nesting and a hash of its signature, and
deliberately holds no source positions and no syntax handles — both of which
shift when a body is edited. Editing a function body therefore reparses one
file and rebuilds one item tree, both of which come out equal, and nothing
else runs.

Two consequences worth knowing:

- A cached value must never carry a source position. Positions belong to the
  revision they were taken in, so anything that reports a diagnostic resolves
  its spans against the current syntax tree. `ResolvedImport` stores an
  ordinal, and `ModuleScope` stores item tree indices, for exactly this reason.
- The same query engine backs the compiler, the language server and the editor,
  so there is exactly one implementation of every rule.

## Packages, modules and the on-disk cache

A **package** is a directory with a `wolang.toml` manifest. A **module** is a
directory of `.wo` files inside it, named after the package plus the path
below the source root: `src/engine/math.wo` in package `game` belongs to
module `game.engine`. Directory-scoped modules mean adding a file never
requires editing a module declaration.

Package discovery touches the filesystem, so it is *not* a query: a workspace
is loaded explicitly, and loading one bumps the `Workspace` input.

The cache stores item trees and nothing else. Each entry carries the hash and
size of the text it was computed from, and the file carries a format version,
so a stale, corrupt or foreign cache is discarded rather than trusted. A
loaded entry is installed through `QueryEngine::Seed`, which records the
file's text as its only dependency — so a seeded value behaves exactly like a
computed one and an edit invalidates it normally.

## Core data structures

| Type | Shape | Why |
|---|---|---|
| `Symbol` | `uint32` index into a `StringInterner` | name comparison is one integer compare |
| `FileId`, `NodeId`, `TypeId`, `FuncId`, `BlockId`, `ValueId` | `uint32` indices into flat arrays | compact, cache friendly, serializable |
| `SourceSpan` | `{FileId, uint32 begin, uint32 end}` | 12 bytes; line and column are derived on demand |
| `Token` | 16 bytes, payloads in side tables | a file of tokens stays in cache |
| `Ast` | one flat array per node category, child lists as ranges into per-category index pools | the whole tree of a file is contiguous and serializable as-is for the query cache |
| `Arena` | bump allocator, no destructors | per-file and per-function lifetimes are free to drop |

Types are interned, so type equality is a `TypeId` comparison.

## The IR

A typed SSA representation, and the contract between the front end and every
back end. Four properties shape it:

**Block parameters instead of phi nodes.** A value that depends on which edge
was taken is a parameter of the block, passed by the jump. A phi node is the
same information written where it is easy to get wrong; as a parameter it
becomes an arity and type check the verifier already does. The verifier's
`RejectsAUseThatIsNotDominated` test is the case this makes safe.

**One id space for instructions and values.** An instruction *is* the value it
produces, so "where did this come from" is a lookup rather than a search. A
pass never renumbers: a dead instruction becomes an `undef` nothing uses, and
a removed block is emptied rather than erased, because an id is an index.

**Flat arrays with side pools.** Instructions, operands, successors and switch
cases are separate vectors, the same layout the AST uses. `Builder` is what
keeps them consistent — a hand written lowering gets that wrong.

**Erasure at the boundary.** Dimensions and open literal types are
checking-time notions, and neither exists in the IR: `Length` is `f32` and a
bare `1.0` has settled. Erasing once, at lowering, is what makes "unit safe
code costs nothing at runtime" true rather than aspirational.

Effect rows are the exception — they are *kept*. "This call does nothing
observable" is what licenses deleting it as dead, so the information has to
survive into the IR rather than being erased with the units.

### Lowering

Lowering trusts the checker: it reads the types the checker recorded rather
than working anything out again, and it only runs on a file that produced no
errors. That division is what keeps the IR free of "maybe" cases.

| Construct | Becomes |
|---|---|
| `if`, `while`, `loop` | blocks, with a parameter on the join when the `if` produces a value |
| `&&`, `\|\|`, `??` | control flow, because the right side may not run |
| `match` | arms tested in order, ending in `unreachable` when the checker proved coverage |
| `T?` | `opt.present`, `opt.value`, `opt.wrap` |
| a `var` | an `alloca` with loads and stores |
| a field | `extract` by index |

Every `var` goes in a slot and a pass promotes the ones that can be. Deciding
during lowering would mean tracking liveness there; leaving it to a pass keeps
lowering simple enough to be obviously correct.

### The verifier

Runs on **every** input, not on request, because the IR is a contract and a
violation is a compiler bug. It checks block shape, dominance, edge arity and
types, reachability, operand types, and a call against its callee's signature.

Dominance uses the iterative set algorithm rather than Lengauer-Tarjan: it is
slower and far easier to be sure of, which is the right trade for something
that has to be correct rather than fast.

The IR is verified **twice** — as lowering left it, and after the passes —
because a pass that breaks an invariant and a lowering that breaks one are
different bugs and have to be distinguishable.

## The interpreter

The reference semantics (D4), not a stepping stone. Every later tier is a
lowering of the same IR and is checked against this.

**A trap is a defined outcome, not a crash.** Division by zero, integer
overflow, an index outside an array, reading an absent optional, reaching an
`unreachable`: each stops execution with a named kind and a location. That is
what makes "this program faults" something the JIT and the AOT tier can be
compared against.

**Overflow traps rather than wraps.** A language that wraps silently turns an
arithmetic bug into a wrong answer somewhere else. The consequence reaches
the optimizer: arithmetic is not pure, so dead-code elimination cannot remove
an unused `a + b` without proving it cannot overflow.

**The step limit belongs to the run, not the program.** A `comptime`
evaluation runs inside the compiler, so it gets a small budget; a program run
gets a large one. Either way an endless loop stops being the compiler's
problem.

**Values are simple rather than compact.** An integer is 64 bits with its
declared width remembered separately, so wrapping is a decision taken in one
place. An f32 rounds through a `float` after every operation, because doing
f32 arithmetic at double precision would make this tier disagree with every
other one.

### Differential execution

`wolang run` executes the program twice — on the IR as lowering left it, and
after the passes — and compares the result, the trap and what was printed. A
difference is a miscompile, which is the most serious bug this compiler can
have, so it is checked on every run rather than in a nightly job. Every
interpreter test does the same.

That check is what found dead-code elimination removing a division by zero.

### The platform floor

A body-less declaration whose name matches an intrinsic is implemented by the
host: `Print`, `ConcatStr`, `IntToStr`, `FloatToStr`, `BoolToStr`. The list
is deliberately tiny — everything the language can say about itself is
written in Wolang, and `lib/core` is where that starts.

An interpolated string lowers to conversions and concatenations of these,
which is why `lib/std/src/io` declares them even though source code rarely
names them.

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
offer fun Integrate(dt: Seconds, q: Query<mut Transform, Velocity>)
offer fun LoadLevel(path: str) -> Result<Level> uses io, alloc
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
| `include/wolang/syntax`, `src/syntax` | token vocabulary, lexer, syntax tree, parser, tree printer |
| `include/wolang/query`, `src/query` | the query engine and the query catalog |
| `include/wolang/compiler`, `src/compiler` | item trees, the database, manifests, modules, workspace loading, the cache |
| `include/wolang/format`, `src/format` | the formatter |
| `tools/wolang` | the command line driver |
| `tests/harness` | the dependency-free test harness |
| `tests/unit` | unit tests mirroring the source tree |
| `examples` | `.wo` sources, also used as compiler corpus |

Planned directories, one per wave that introduces them: `sema`, `check`, `ir`,
`opt`, `interp`, `codegen`, `runtime`, `object`, `interop`, `ecs`, `net`.
