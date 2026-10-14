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
survive into the IR rather than being erased with the units. A row carries a
second bit, `effects_known`: an empty row is an *assertion* (D38), so a
function whose row was never inferred — a declaration from another file —
has to read as unknown rather than as one that does nothing.

What the row does **not** yet license is the removal it was kept for. An
effect row says nothing about trapping, and deleting a call that would have
faulted turns a faulting program into one that does not — the same argument
that makes arithmetic impure (D15c). The row becomes actionable when the IR
can also say "this function cannot trap".

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

**A generic function is not lowered at all.** It is a recipe, and emitting it
would put a `T` in the IR that every back end would then need an answer for.
Instead the checker records what each generic call was inferred to be, and
lowering emits one specialization per instantiation, named after its
arguments: `Identity<s32>`. Two call sites that inferred the same arguments
reach the same copy. A generic body calling another generic passes its own
parameters along, so the inner instantiation only becomes concrete when the
outer one does — which makes the worklist, not a second pass, the right
shape.

### The passes

| Pass | Does |
|---|---|
| `InlineCalls` | replaces a call to a small function with the body it calls |
| `FoldConstants` | arithmetic and comparison on two constants |
| `PromoteSlots` | turns a slot into values, inserting block parameters where a value depends on the path taken |
| `SimplifyControlFlow` | a constant branch becomes a jump, a block one block reaches is merged into it, unreachable blocks go |
| `EliminateDeadCode` | drops what nothing reads and nothing can observe |

Two of them are where the id design pays off. Inlining splices the callee's
blocks in and turns the **call instruction itself into the parameter** of the
block control comes back to: every operand that read the call's result now
reads the parameter, and nothing is rewritten. Merging a block into its one
predecessor substitutes the block's parameters with the values the jump
carried, which is what lets a block with parameters be merged at all — and
an inlined body always comes back through one.

Slot promotion is SSA construction: a loop counter written every iteration
becomes a block parameter on the loop header. It is what the block-parameter
form was chosen for, and it is the difference between a loop that keeps its
variables in registers and one that makes a round trip through the frame
every iteration — measured at three times the speed on the benchmarks. It
declines in one case: a block that needs a parameter whose predecessor ends
in a branch rather than a jump, because only a jump can carry arguments and
this pass does not split edges.

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

## The dev tier

Copy-and-patch code generation (D5), for **arm64 and x86-64**. Each is an
assembler and an emitter and nothing else: the IR, the passes, the layout,
the trap table and the transform that splits a `task` into a state machine
are all above the seam in `codegen/target.h`. Both assemblers' encodings are
checked against a system assembler's, because an encoder that is almost right
produces code that almost works and nothing further up can tell. That check
has now caught four wrong expectations and no wrong encodings, which says
something about where to put the trust.

What differs between the two is mostly the shape of an instruction. Every A64
instruction is four bytes, so "a stencil with holes for the operands" is a
word and a bit field; an x86-64 instruction is one to fifteen bytes of
prefix, opcode, ModRM, SIB and displacement, so the seam counts in bytes and
each target patches a call displacement its own way.

**Linear scan from the start** (D39), not accumulator stencils. The allocator
hands out **callee-saved registers only**. That is what makes a call need no
spilling around it: every value the allocator placed survives a call because
the callee is obliged to restore it. The prologue saves the ones a function
used.

How many registers that leaves is a fact about the platform, not a choice:

| | integer | floating point |
| --- | --- | --- |
| arm64 | 9 | 8 |
| x86-64 | 4 | **0** |

There are no callee-saved floating point registers in the x86-64 ABI at all,
so on that machine every float value lives in a frame slot and is loaded
around each use. It is slower and it is correct, which is the right order to
do these in; spilling only the values that are live across a call is what
would fix it, and that is a change to the allocator rather than to either
emitter. x86-64 also reserves two registers rather than fighting over them: a
variable shift needs its amount in `cl` and a division needs `rdx:rax`.

Two things that tier declines on x86-64 and takes on arm64: a conversion
between a float and a 64-bit *unsigned* integer, because both of the
machine's conversion instructions are signed and the correction sequence is
not written yet; and a call with more than six integer arguments, because
that is how many this ABI passes in registers. Both leave the function to the
interpreter, which is always there and always right.

An aggregate never gets a register: a struct, a tuple, an array and an
optional each live in a frame area, and the value of such an SSA name is its
address. Aggregates are passed and returned by pointer, which for the return
is also what the platform ABI does. The cost is a copy the optimizing tier
will want to avoid; the benefit is that one rule covers every aggregate
however it was produced.

**A trap in compiled code is the interpreter's trap.** Each site carries the
kind, the place and the message the interpreter would have produced, and the
check before the operation branches to a three-word stub that calls the host.
Reporting is a non-local exit — the host records the trap and leaves through
a landing pad — so the path that does not fault pays for nothing but the
untaken branch. The checks are the answer to this wave's open question:
portable and predictable, with the plan to remove them where range analysis
proves them unnecessary, which is the same analysis dead arithmetic needs.

**How a second backend is tested on a machine that is not one.** The
`x64` preset builds the whole compiler for x86-64; on an arm64 Mac the
result runs under the system's translator, which translates the code the
dev tier generates at run time along with everything else. So the full
suite — including the differential tests that run every program three
ways — exercises the x86-64 emitter on this hardware, and that is how it
was verified. What that cannot measure is speed: a benchmark there times
the translator, not the backend.

```sh
cmake --preset x64 && cmake --build build/x64 -j8
./build/x64/tests/wolang_tests
```

Every compiled function builds a frame record whether it needs one or not,
which is the whole unwind information this tier needs: a trap walks the chain
of frame pointers and symbolizes each return address, so a fault deep in
compiled code names its callers.

The tier **compiles what it can and says what it did not**. A function that
holds a string, calls something uncompilable, or wants more argument
registers than exist is left to the interpreter, and `wolang asm` prints the
reason. The interpreter is the reference semantics and always available, so
declining is cheap and guessing would not be.

### How it is known to be right

Three tiers run the same program on every test: the interpreter on the
unoptimized IR (the reference), the interpreter on the optimized IR (did the
passes preserve meaning), and the compiled code (does this tier agree).
`wolang test` does the same for every `@Test` in a package — the tests of
`lib/core` run in both tiers and are compared.

`benchmarks/run.sh` compares the four gameplay kernels against the same
kernels in C++, twice: against `-O2`, and against `-O2` with vectorization
off. The second column is the one that measures this tier's code quality; the
first includes a loop clang turns into vector code, which a scalar tier is
not trying to be within 3x of.

## Places: where a value lives

A field of a struct, an element of a buffer, a method's receiver and what a
pointer points at are all the same question -- *where is this value* -- and
lowering answers it once, in one place. A **place** is a root and a path of
steps: the root is a slot, a pointer, or a value with no location at all,
and each step is a field (which the layout knows) or an element (which only
the run knows).

Reading and writing both go through that path, so `o.inner.n = 42` is one
store at the address the path works out rather than a copy of `o`. Which
matters more than it sounds: before this existed, assigning to a field
compiled to *nothing at all*, and so did assigning to an element.

The interpreter has no addresses -- its values carry their shape instead --
so a place there is literally a path: which frame, which slot, and the steps
from it. An element step adds to the step before it, the way pointer
arithmetic does; a field step goes inwards. It names its frame because a
slice of a local array is passed to callees, and holding the frame is safe
for the same reason the escape rule exists.

A **slice** is this model's payoff: a pointer and a length, with the length
as an operand of the element-address instruction rather than a property of
the type. One bounds check covers an array, a slice and a borrowed string,
and no tier has to know which one it is looking at.

## Tasks

A `task` is stackless, and the whole of that decision is one transform.

Everything that outlives a suspension goes in a **frame**: the state it left
off at, its parameters, its result, one field per slot the body keeps, and
one field per `wait` for the frame of what it waits on. The function becomes
`F$resume(frame) -> bool` with a switch over where it left off, and a `wait`
becomes "poll the callee; if it is not finished, record the state and
return". After the transform there are no tasks -- only functions and
structs -- which is why the interpreter and the code generator need to know
nothing about suspension, and why the differential check between them covers
coroutines for free.

Three consequences are worth knowing:

- **A frame costs bytes, not a stack.** A task that waits four times keeps
  twenty bytes. A hundred thousand of them is two megabytes, which is the
  difference between a benchmark and an impossibility.
- **The callee's frame lives inside the caller's.** A static call graph needs
  no allocation per `wait`, and recursion is rejected (`E0304`) rather than
  silently needing an unbounded frame.
- **The frame layout is computed per file**, so a `wait` on a task from
  another module is `E0305`. A task with no body -- `std.tasks.Yield` -- is
  the exception and the suspension primitive: the compiler writes its
  machine, which suspends once and then finishes.

`spawn` allocates a frame, writes the arguments into it, and hands the pair
to the scheduler in `runtime/`. The scheduler holds *kinds and frames*, never
function pointers: the transform generates one `wolang$dispatch(kind, frame)`
for the whole module, so the same scheduler advances a task whether the
program is being interpreted or has been compiled. Round robin, cooperative,
single threaded; parallelism is a later wave.

## The C boundary

An `extern` declaration is implemented outside the language, reached through
the C ABI. Three things make it work and one sentence makes it safe.

**Calling out of the language is the `unsafe` effect.** Every rule already
in place then applies — `offer` has to state it, a `@Deterministic` context
cannot reach it, and a package with `unsafe = "deny"` cannot call out at all
— and the FFI adds exactly one diagnostic of its own: `E0802`, for a type
with no C representation. The boundary takes numbers, `bool` and pointers,
because everything crossing it has to mean the same on both sides.

A declared `uses unsafe` **absorbs** the obligation (D58), which is what
makes a safe wrapper over an unsafe primitive possible. `std.io.Write` is
that wrapper; `std.io.Print` calls it and is not unsafe.

**Both tiers reach C the same way.** Compiled code calls the address the
loader gave; the interpreter calls through a thunk generated with the same
assembler, which loads the argument registers out of two arrays. One thunk
covers every signature the boundary allows, because the only thing a
signature changes is how many registers hold something. Without it, every
program using the FFI would fall outside the differential check.

**The dev tier compiles a program, not a file.** Lowering is per file, so a
call into another file is a declaration with no body; the code generator
takes every file at once and links a declaration to its definition by the
declaration it came from — a file and an item index — rather than by name,
because two files may each define a private `Helper`.

## Memory model

Decided in wave 0, implemented in wave 9:

- `struct` is a value type with mutable value semantics; copies are copies.
- References are **second class**: they may be parameters and locals but not
  fields, which gives memory safety without a borrow checker.
- `Owned<T>` is unique ownership with compile-time move checking.
- `Shared<T>` is shared ownership; a thread-safe variant comes with the concurrency wave.
- `arena { ... }` is an arena whose values may not escape it, checked by
  escape analysis.
- `class` is a garbage collected reference type, and the collector is a
  separate optional component. A program that declares no `class` links no
  collector, which is what makes WASM and embedded targets practical.

## Effects and capabilities

A function's effects are inferred inside a module and written explicitly on
public signatures:

```wolang
offer fun Integrate(dt: Seconds, q: Query<mut Transform, Velocity>)
offer fun LoadLevel(path: text) -> Result<Level> uses io, alloc
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
| `include/wolang/sema`, `src/sema` | types, dimensions, name resolution, type checking, effects |
| `include/wolang/ir`, `src/ir` | the typed SSA IR, lowering, the verifier, the passes, the printer |
| `include/wolang/interp`, `src/interp` | the interpreter, values, intrinsics |
| `include/wolang/sema/flow.h`, `src/sema/flow.cc` | definite assignment, moves and reference escape -- one walk, three rules |
| `include/wolang/abi`, `src/abi` | how a value is laid out in memory: what the emitter, the C boundary and the interpreter all have to agree on |
| `include/wolang/codegen`, `src/codegen` | register allocation, the target seam, the arm64 and x86-64 assemblers and emitters, the runtime a trap reports through |
| `include/wolang/runtime`, `src/runtime` | what a running program needs from the host: task frames and the scheduler |
| `benchmarks` | the gameplay kernels, their C++ counterparts, and the script that compares them |
| `lib/core`, `lib/std` | the standard library, written in Wolang |
| `tools/wolang` | the command line driver |
| `tests/harness` | the dependency-free test harness |
| `tests/unit` | unit tests mirroring the source tree |
| `examples` | `.wo` sources, also used as compiler corpus |

Planned directories, one per wave that introduces them: `object`, `interop`,
`ecs`, `net`.
