# Design decisions

Every decision that shapes the language or the compiler, with the reason.
Numbered so that code comments and `STATUS.md` can point at one. A decision is
revisited only when something new is known, not because it was inconvenient.

## The project

**D1. Wolang is its own project, not an engine module.** The language is for
general use; a game engine is one host among several, and its adapter arrives
in wave 15. Keeping the core engine-free is what stops engine concepts from
leaking into the type system.

**D2. The compiler stays C++20 — self-hosting is not a goal.** Rewriting the
compiler in Wolang needs the finished language first and doubles the work for
years. It remains an option after phase D, not a commitment.

**D3. The standard library is written in Wolang** above a thin non-Wolang
floor: a C++ platform layer for syscalls, and compiler intrinsics for what the
language cannot express (`memcpy`, atomics, SIMD, `sqrt`). `core` becomes
possible in wave 7, when the interpreter can run it.

**D4. The interpreter is the reference semantics**, not a stepping stone. The
JIT, the AOT tier and the WASM backend are mechanical lowerings of the same
verified IR, and differential fuzzing across all four is what keeps them
honest.

**D5. arm64 before x64** for the dev-tier code generator, because that is the
development machine.

**D6. Chromium C++ style everywhere, no exceptions and no RTTI.** Commits are
Conventional Commits, one line, no body, no trailers.

**D7. `docs/` is in English, `STATUS.md` is in Russian.** The docs are public
artifacts of the language; `STATUS.md` is the working log.

## Memory

**D8. Hybrid memory, optional collector.** Values with mutable value
semantics; references are second class, so they may be parameters and locals
but not fields — memory safety without a borrow checker. `Box<T>` for unique
ownership, `Rc`/`Arc` for shared, `region { }` for frame arenas, and `class`
with a garbage collector as a **separate optional component**. A program that
declares no `class` links no collector, which is what makes WASM and embedded
targets practical.

## Types

**D9. Integer literals have arbitrary precision until used.** A literal takes
the type its context expects, defaulting to `int`. `0xFFFFFFFFFFFF` needs no
suffix, `comptime` arithmetic is exact, and a literal that does not fit its
destination is an error rather than a silent wrap.

**D10. `[T; N]` and `[T]` are language constructs; `Array<T>` is a library
type.** Fixed arrays and slices allocate nothing and `core` needs them, so
they belong to the language. A growable array needs an allocator, so it
belongs to `std` (wave 13).

**D11. `T?` *is* `Option<T>`.** One concept, one implementation, flow typing
that works the same for values and references. The compiler niche-optimizes
it, so `Actor?` is a nullable pointer at runtime.

**D12. Optionals need no `Some`/`None` vocabulary.** The language already
spells presence and absence: `T?` in a type, `x?` in a pattern, `null` for
absence, `??` for a default. Importing two more words for a concept already
expressible is a cost with no benefit.

**D13. Error sets are inferred, and `T!` is postfix.** `fun Load() -> Level!`
collects the errors the body can produce, so `?` never needs a conversion
between error types. Postfix matches `T?`, so a type reads left to right and
the two combine as `Pawn?!`. An error is a tag without data; a payload means
writing `Result<T, E>` explicitly.

**D14. Enum variants are members of their type, with a short form in
`match`.** `Shape.Circle(r)` everywhere, and bare `Circle(r)` inside a `match`
whose scrutinee type is known. No name collisions between enums, and no type
name repeated on every arm.

**D15. No `fixed` type.** `@Deterministic` code puts floats in a strict mode:
no FMA contraction, no fast-math, fixed operation order. IEEE-754 is itself
deterministic — the nondeterminism comes from the compiler, not the hardware.
A library `Fixed<N>` can arrive in wave 13 if measurements call for it.

**D15a. There are no implicit numeric conversions, not even widening.**
`let n: i64 = some_i32` is an error; `some_i32 as i64` is how it is written.
Arbitrary-precision literals (D9) remove the common case that would
otherwise make this tiresome, and a conversion that is always spelled is a
conversion that can always be found. This retires the planned `W0114`
"implicit narrowing" lint: there is no implicit narrowing to warn about.

## Syntax

**D16. Control flow is expression-shaped.** `if`, `match`, `loop`, `while`,
`for`, `foreach`, `unsafe`, `region` and blocks are all expressions, so
`let x = if c { a } else { b }` needs no special case.

**D17. An operator at the start of a line begins a new statement.** A line
starting with `-` is a unary minus, never a continuation, so two statements
can never silently fold into one. `.` and `?.` are exempt, because
leading-dot method chains are normal style.

**D18. No turbofish.** `f<T>(x)` is resolved by scanning ahead for balanced
angle brackets followed by `(`. The cost is that a function type cannot be an
explicit generic argument in expression position; both affected forms are rare
enough to be worth the simpler rule.

**D19. A dot in an expression is always member access.** `World.Spawn` becomes
a path during name resolution, which is what languages without a `::`
separator do.

**D20. `as` is the cast operator**, binding tighter than any binary operator.

**D20a. `fun`, `need` and `offer` replace `fn`, `import` and `pub`.** `fun` is
the whole word rather than an abbreviation. `need` and `offer` are a matched
pair that says what a module does: it *needs* names from elsewhere and
*offers* its own. No other language uses this pair, which is the point — the
words describe the relationship rather than borrowing a convention. `fn`,
`import` and `pub` are reserved, so old habits get a message.

**D21. `extend T` and `extend T as Trait` replace `impl`.** The target comes
first either way, so the declaration reads as a sentence. `impl Trait for
Type` is a known stumbling block: its word order matches neither English nor
intuition. `impl` is reserved, so the Rust spelling gets a message rather than
a confusing parse failure.

**D22. Storage layout is a property of the storage, not of the type.**
`soa [Particle]` rather than `@layout(soa) struct Particle`. This is a
capability difference, not a spelling one: the same type can be one value in
one place and a struct-of-arrays in another, and the choice can be flipped
where it is measured.

**D22a. A derived dimension is declared, not written inline.**
`dimension Velocity = Length / Time` names the product once; there is no
`Length/Time` syntax in general type position. A dimension name then *is* a
type — the type of quantities carrying it — so `speed: Velocity` is how a
velocity is declared. Confining the arithmetic to one declaration keeps type
syntax free of operators and makes the concept nameable.

**D23. Declaration-introducing words are contextual keywords** — `state`,
`unit`, `machine`, `derived`, `effect`, `dimension`, `enter`, `exit`,
`always`, `with`, `without`, `soa`, `aos`. Gameplay code names variables
`state` and `unit` constantly; stealing those spellings would be a daily cost
for a one-time grammar convenience.

**D24. A unit suffix on a number is just a symbol.** Units are user declared,
so the lexer records the spelling and sema resolves it.

**D25. Block comments nest.** The only reason to reach for `/* */` is to
comment out a region, and a region often already contains a comment.

**D26. Identifiers are Unicode XID and must already be in NFC.** Letters from
any script, but no emoji, symbols or invisible characters — a zero-width space
in a name would hide one name inside another. Non-NFC names are rejected
rather than normalized, which is what guarantees one name has one spelling. A
combining mark that cannot compose is accepted, so Devanagari, Thai and
Vietnamese names work.

## Modules and the build

**D27. A module is a directory, not a file.** Its name is the package name
plus the path below the source root. Adding a file never requires editing a
module declaration, and the layout matches how a game splits into plugins.

**D28. The manifest is declarative TOML.** External tools — CI, a registry, a
dependency bot — must be able to read a package's name and version without a
Wolang interpreter. A build script (`build.wo`) can arrive *beside* the
manifest in wave 7+, exactly as `build.rs` sits beside `Cargo.toml`.

**D29. Package discovery is not a query.** The query engine can only track
facts it is told about, and the filesystem is not one of them. A workspace is
loaded explicitly, and loading one bumps the `Workspace` input.

## Incremental compilation

**D30. Query dependencies are discovered, not declared.** Every `Get()` a
computation makes is recorded, so a computation and its dependency list can
never drift apart.

**D31. A cached value may never carry a source position.** Positions belong to
the revision they were taken in; anything that reports a diagnostic resolves
its spans against the current syntax tree.

**D32. The item tree holds no positions and no syntax handles.** Both shift
when a body is edited, and an item tree that changes after a body-only edit
would defeat its own purpose.

**D33. The cache stores item trees only, and is never trusted.** Tokens and
trees are large and are needed only when the file is about to be read anyway.
A stale, corrupt, truncated or foreign cache is discarded.

**D34. A file outside a package resolves no imports.** Checking a single file
is a legitimate thing to do and must not invent errors.

## Effects

**D35. Effects and capabilities are one mechanism**, inferred inside a module
and explicit on `offer` signatures. One mechanism covers determinism,
allocation-free hot paths, mod sandboxing, `comptime` purity and server/client
separation — five unrelated attribute checks collapse into one.

**D35a. Effect inference is one query per module, not one per declaration.**
A per-declaration query turns mutual recursion into a query cycle, and the
engine's cycle detection would then give an answer that depends on which
declaration was asked about first. Order-dependent results in an incremental
compiler are not a performance problem, they are a correctness one. The
fixpoint therefore runs over a whole module at once, and reporting is a third
query so that type checking — which collects the rows — does not depend on
the module-wide answer it feeds.

**D35b. An `offer` function with effects must state them.** Inference stops
at the module boundary, because inferring across it would need
`ModuleEffects(A)` to ask `ModuleEffects(B)` and back again. Stating the row
on the signature is what the boundary buys, and it is where a reader looks
anyway.

**D35c. An empty effect row is a claim, not ignorance.** `comptime` needs to
say "nothing observable"; "not stated" is the absence of an entry. Conflating
the two would make every unannotated function look pure.

**D35d. Trait conformance is declared, not structural.** `extend T as Trait`
is the declaration. A type that happens to have the right methods has not
promised anything, and a bound is a promise.

## Runtime policies

**D36. Hot reload restarts an incompatible coroutine.** Predictable and
explainable in one sentence, which matters more for editor iteration than
preserving state. `@HotReload(FinishOld)` or `@HotReload(Cancel)` overrides it
per function.

**D37. A fault in a shipping build isolates the culprit.** The coroutine or
system that faulted stops, the rest of the game keeps running, and a report
goes to telemetry. One broken enemy beats a drop to the menu. A project
switches the policy in its manifest; a server usually should.

**D38. Mods ship as `.wo` source.** Readable, reviewable, patchable, and
learnable from — which is how strong modding communities actually work.
Compiled by the dev tier on the player's machine, or by the IR interpreter
where a JIT is not allowed.

**D39. The dev tier gets a real register allocator (linear scan) from the
start.** Longer to build than accumulator stencils, but better code from day
one rather than a rewrite once the benchmarks disappoint.

**D40. An HTTP server is a first-party library, and it starts at wave 10 as
the driver for the async design — not at the end as a showcase.** A server is
the harshest test a concurrency model gets: thousands of tasks, each mostly
waiting, with cancellation and deadlines everywhere. Writing a minimal
HTTP/1.1 server *while* structured concurrency is being designed is what
keeps the design honest; writing it afterwards only proves the design was
already fixed.

It lives in its own package (`wolang-http`), consumed through a path
dependency, which also exercises the package graph. The transport layer —
kqueue, epoll, io_uring — belongs in `std` (wave 13), not in the HTTP
library: a server is one user of readiness notification, not its owner.

Three language features make it unusually good rather than merely possible:
a `region` per request is arena-per-request, which is how the fastest
servers already work but spelled as a language construct; effects and
capabilities give per-handler sandboxing checked at compile time, so a route
declared `uses net` cannot touch the filesystem and a plugin route can be
granted `capabilities = "net"` and nothing else; inferred error sets mean
`fun Handle(r: Request) -> Response!` without a hand-written error enum.

TLS is bound, not written — BoringSSL or rustls through the C ABI (wave 9).
Nobody should write TLS, and pretending otherwise would be the one decision
here that could not be walked back.
