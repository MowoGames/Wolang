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
but not fields — memory safety without a borrow checker. `Owned<T>` for unique
ownership, `Shared` for shared, `arena { }` for frame arenas, and `class`
with a garbage collector as a **separate optional component**. A program that
declares no `class` links no collector, which is what makes WASM and embedded
targets practical.

## Types

**D9. Integer literals have arbitrary precision until used.** A literal takes
the type its context expects, defaulting to `int`. `0xFFFFFFFFFFFF` needs no
suffix, `comptime` arithmetic is exact, and a literal that does not fit its
destination is an error rather than a silent wrap.

**D10. `[N]T` and `[]T` are language constructs; `Array<T>` is a library
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
`const n: s64 = narrow` is an error; `narrow as s64` is how it is written.
Arbitrary-precision literals (D9) remove the common case that would
otherwise make this tiresome, and a conversion that is always spelled is a
conversion that can always be found. This retires the planned `W0114`
"implicit narrowing" lint: there is no implicit narrowing to warn about.

**D15b. An owned `Text` is accepted where a borrowed `text` is wanted.** An
implicit borrow, and safe for the reason references are second class (D8): a
`text` cannot be stored in a field, so the view can never outlive the string
it points into. Without this, printing an interpolated string — the most
common operation in any language — would not type check, and requiring a
sigil for it would buy nothing a borrow checker is not already providing.

**D15c. Arithmetic is not pure, because overflow traps.** Integer overflow
and division by zero are traps, and a trap is a defined outcome the program
can observe. Deleting an unused `a + b` would therefore turn a program that
faults into one that does not, so dead-code elimination may not remove it
without a proof that it cannot trap. That proof is range analysis and
belongs with the optimizer proper. The alternative — wrapping silently, so
that arithmetic becomes pure — buys optimizations by turning an arithmetic
bug into a wrong answer somewhere else.

## Syntax

**D16. Control flow is expression-shaped.** `if`, `match`, `loop`, `while`,
`for`, `foreach`, `unsafe`, `arena` and blocks are all expressions, so
`const x = if c { a } else { b }` needs no special case.

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
`soa []Particle` rather than `@layout(soa) struct Particle`. This is a
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
a `arena` per request is arena-per-request, which is how the fastest
servers already work but spelled as a language construct; effects and
capabilities give per-handler sandboxing checked at compile time, so a route
declared `uses net` cannot touch the filesystem and a plugin route can be
granted `capabilities = "net"` and nothing else; inferred error sets mean
`fun Handle(r: Request) -> Response!` without a hand-written error enum.

TLS is bound, not written — BoringSSL or rustls through the C ABI (wave 9).
Nobody should write TLS, and pretending otherwise would be the one decision
here that could not be walked back.

## Code generation

**D41. `as` converts between numbers and does nothing else, and a narrowing
conversion traps.** D33 removed implicit widening, so `as` is the only
numeric conversion there is — not a reinterpretation of bits, which is what
`unsafe` is for. Converting anything that is not a number is `E0124`. A
conversion that cannot hold the value traps rather than keeping the low
bits, because overflow traps everywhere else in this language and a cast
that quietly changed a number would be the one place it does not.

**D42. A generic function is never lowered; one specialization is emitted per
instantiation.** A generic declaration is a recipe, and lowering it would put
a `T` in the IR that every back end would then need an answer for — the
verifier rightly rejects a call passing an `s32` where a `T` is declared. The
specialization is named after its arguments, so two call sites that inferred
the same arguments share one copy. Cross-file instantiation needs
whole-program lowering and waits for the linking story.

**D43. A trap in compiled code is a check before the operation, not a signal
handler.** This was wave 8's open question. A check costs an instruction and
an untaken branch on every arithmetic operation; a handler is free on the hot
path but is tied to the platform, cannot work for WASM, and makes every
arithmetic instruction a potential non-local exit. The check is portable and
predictable, and the plan is to remove it where range analysis proves it
unnecessary — the same analysis that would let dead arithmetic be deleted
(D15c). Paying for a check that an analysis can later remove is a better
place to be than depending on a mechanism one target cannot have.

**D44. The register allocator hands out callee-saved registers only.** Nine
integer registers and eight floating point ones instead of twenty-odd, and in
exchange a call needs no spilling around it: every value the allocator placed
survives a call because the callee is obliged to restore it. The prologue
saves what the function used. For a dev tier that compiles fast and has to be
right, removing the caller-save problem is worth more than the extra
registers.

**D45. An aggregate lives in the frame and is passed by pointer.** A struct, a
tuple, an array and an optional never get a register in this tier: the value
of such an SSA name is an address. One rule then covers every aggregate
however it was produced, and the return convention coincides with the
platform's indirect return. Scattering fields across registers is an
optimizing-tier decision.

**D46. A trap is reported by a non-local exit.** Generated code calls the host
through the one pointer it keeps, and the host records the trap and leaves
through a landing pad established when compiled code was entered. A generated
frame holds machine values and nothing that needs unwinding, so leaving
through one costs nothing — and the alternative, a check after every call,
would be paid on the path that does not fault. The context has to live
outside automatic storage: an object changed between `setjmp` and `longjmp`
has an indeterminate value afterwards, and the optimizer is entitled to fold
the trap kind back to the zero it was initialized with. It did.

**D47. The trap vocabulary belongs to the IR, not to the interpreter.** A
division by zero is a property of the operation. Both tiers have to name it
identically or the differential check between them would be comparing two
vocabularies.

**D48. A trap message says only what both tiers know.** The interpreter could
print the offending index; compiled code cannot put a runtime value into a
string it built while compiling. Keeping the dynamic value out of the message
is what makes the two tiers' outcomes comparable in full — and the value is
in the frame, which is where a debugger looks for it.

**D49. Every compiled function builds a frame record, even a leaf.** Two
instructions in the prologue, and in exchange a stack walk is two loads per
frame and needs no unwind table at all. A trap deep in compiled code can
name its callers, which is what makes a fault in generated code debuggable
before there is a debugger.

**D50. The dev tier compiles what it can and says what it did not.** A
function holding a string, calling something uncompilable, or wanting more
argument registers than exist is left to the interpreter, and `wolang asm`
prints the reason. The interpreter is the reference semantics and is always
there, so declining is cheap — and a tier that guessed would be the one thing
nobody could debug.

**D51. `wolang run` stays on the interpreter until the FFI arrives** —
*settled in wave 9*: the FFI arrived, `std.io.Print` is written in Wolang,
and `run` uses the compiled tier when it can. `--interpret` forces the other
one. The reasoning is kept because it is why the dev tier was exercised
through `wolang test` first. Any
interesting `main` prints something, and printing goes through an intrinsic
the dev tier cannot call yet. The compiled tier is exercised where it is
honest to: `wolang test` runs every `@Test` in both tiers and compares, and
`wolang bench` times it. Running `main` through compiled code would also
quietly drop the step limit, which is a property of the run and not
something a tier gets to change.

## The boundary

**D58. A declared `uses unsafe` absorbs the obligation rather than
re-exporting it.** `unsafe` is not a capability like `io` or `net` — those
say what the code needs from the outside world, and a caller has to know.
`unsafe` says someone checked, and the declaration that states it is the one
taking responsibility. Without this there is no such thing as a safe
abstraction over an unsafe primitive: `std.io.Print` would make every
program that prints an unsafe one, and a package with `unsafe = "deny"`
could not print at all.

**D59. A program runs once.** Wave 7 ran every program twice — once on the
unoptimized IR and once on the optimized one — and compared, which was worth
it while the only thing a program could do was fill a sink the driver owned.
Once a program can call `write`, running it twice writes twice. A side
effect is not something to repeat for the compiler's benefit, so the
comparison moved to `wolang test`: every `@Test` runs in both tiers, and a
test is the one place a program is known to terminate.

## Names

**D52. `const` and `var`, not `let` and `var`.** Two visually distinct words
for two different things, rather than one word and a modifier. Mutable
locals are most of the locals gameplay code has, so `var x` must not cost
more to write than the immutable form -- and `const` is free to mean "never
changes" because compile-time evaluation is spelled `comptime`.

**D53. `task fun` and `wait`, not `async` and `await`.** Shorter, and `wait`
says what the machine does rather than naming a protocol. The suspension
point stays visible at the call site, which is the one thing the alternative
-- making suspension an effect in the `uses` row and dropping `wait`
entirely -- would have given up. That alternative is the more interesting
design and may come back when the effect system has the concurrency wave
behind it; until then a reader can see where a function yields.

**D54. `Owned<T>` and `Shared<T>`, and `arena` for a scope.** They say in
English what the relationship is: one owner, several owners, a region freed
all at once. `arena` is also the word the gameplay world already uses for
exactly this allocator.

**D55. `s32`, not `i32`.** Both halves of the width table are integers and
only one of them is signed, so `s` carries the information `i` does not.
Gameplay SDKs have spelled it this way for years. `int` and `float` remain
the aliases for code that does not care about a width, and `f32`/`f64` keep
their spelling because there is no second letter to disagree about.

**D56. `text` and `Text`, not `str` and `String`.** Lowercase for the view,
PascalCase for the type that owns its bytes -- the same rule every declared
type in the language already follows. A view costs nothing and an owned
string allocates, and the capital letter is where that difference is
visible.

**D57. `[]T` and `[N]T`, not `[T]` and `[T; N]`.** The brackets say what the
shape is before the element does, so a type reads left to right: a slice of
`T`, an array of `N` of them. It is also two characters shorter in the array
case, and the semicolon inside a type bracket was the one piece of
punctuation nobody could guess the meaning of.

The words these replaced -- `let`, `async`, `await`, `region` -- are
**reserved** rather than free, so writing one gets a diagnostic instead of a
confusing parse failure.

**D60. Methods come before everything that wants them.** A `fun` inside a
`struct`, a `class` or an `extend` is a member function with `self` as an
implicit first parameter, and a bare name in its body is a member of the
enclosing type -- which section 4.4 of the spec already promised. Until
this wave none of it was lowered, and worse, none of it was *reported*: a
method call produced an undefined value and the checker said nothing. So
methods are built first, and the holes get diagnostics, before signals,
machines or anything else that would be written on top of them.

**D61. A class is a handle to a heap object, and a destructor is what frees
it.** Wave 9 deferred `Owned`/`Shared` because both need "a type that frees
something when it goes out of scope" and `defer` had to exist first. It
does. So instantiation allocates, a binding that holds a handle drops it at
scope exit unless it was moved, and `Owned`/`Shared` are then two types
rather than two mechanisms.

**D62. A signal has both a compile-time and a run-time side.** Connections
that are known statically expand into direct calls at the `Emit`, which
costs nothing and covers the engine's own wiring. Run-time `Connect` over a
list of function values covers mods and anything dynamic, and waits for
function values to be a thing in the language. One word, two
implementations, and the fast one is the default rather than an
optimization.

**D63. A `derived` value caches and is invalidated by a write.** The spec
says it "recomputes when an input changes rather than on every read", which
is a claim about *when*, not a figure of speech: the compiler works out
which fields the expression reads and a write to any of them marks the
cache dirty. A read of a clean cache is a load. The alternative -- a method
without parentheses -- would be simpler and would make the spec's sentence
false, so the sentence stays and the analysis gets written.

**D64. A keyword is a name where only a name can appear.** After a `.`, and
where a declaration gives its own name, a keyword is read as a name:
`o.tag`, `need std.task`, `var state: int`. There is nothing for it to be
ambiguous with in those positions, and the alternative is a language that
makes its users rename their data -- `tag`, `state`, `task` and `component`
are words gameplay code wants. A keyword in an *expression* is still a
keyword, so a field named one is written `self.tag` rather than `tag`.
Making every declaration keyword contextual everywhere would remove that
last wrinkle and is a bigger change to the grammar than it looks.

**D65. One address model, three tiers.** A field of a place, an element of
a buffer and a method's receiver are all "somewhere inside something", and
each tier answers that its own way: the compiled tiers have real addresses,
and the interpreter -- whose values carry their shape rather than living in
memory -- has a *place*: a frame, a slot and the steps to walk from it. An
element step adds to the one before it, because a pointer to element three
plus two points at element five; a field step goes inwards. A place names
its frame, so a slice of a local array can be passed to a callee, and that
is safe for the same reason `E0121` exists.

What this replaced was worse than slow: assigning to a field compiled to
nothing, assigning to an element compiled to nothing, and `for` over a
range lowered to an undefined value. Three silent wrong answers, found by
writing the programs a buffer needs.

**D66. The length of a sequence is an operand, not a property of a type.**
An array knows how long it is and a slice does not, so `index.addr` takes
the length as its third operand and the bounds check reads that. One check
covers an array, a slice and a borrowed string, and no tier has to know
which it is looking at. `.len` is an `int` for the same reason it exists:
it is read in arithmetic and compared with literals far more often than it
is handed to an allocator.

**D67. One string in every tier, built by the runtime.** A string is a
pointer and a length -- interpreted, optimized and compiled alike -- and
what builds one is a call into the runtime that both tiers make. Before
this the interpreter held a C++ string and the compiled tier held a pair,
so a program that formatted anything could only run in one tier, and an
interpolated string dragged its whole function into the interpreter.

Equality is a call for the same reason: two pointers are not an answer to
"are these the same bytes", and comparing the pairs -- which is what
structural equality does -- said that two copies of `"GET"` were different
strings. Both tiers now call `wolang_text_equal`, and the interpreter calls
it directly rather than through a thunk, because the reference tier has to
work on a machine with no backend at all.

**D68. `arena { }` is the allocator, not a promise about one.** Everything
allocated inside the block -- objects, strings -- comes from one arena, and
the whole arena is released at the end, including when the way out is a
`return` or a `break`. One free for the lot, however much was built: a
request handler allocates as it likes and pays once, and twenty thousand
blocks allocating a million objects between them peak at three megabytes.

This is the answer to leaking for now, and a better answer than per-value
destructors for the workload that matters first. `Owned<T>` and a collector
are still ahead; what keeps a value from outliving its arena is `E0121`,
which has been enforced since wave 9.

**D69. A server is a test the language cannot argue with.** D40 put a
minimal HTTP server in this wave to drive the concurrency design, and it
did -- but what it mostly drove was everything *else*. Writing it turned up
ten holes, and every one was a wrong answer rather than a missing feature:
a buffer that a callee filled and nobody could see, a module-level constant
that lowered to an undefined value, a frame the dev tier declined so the
whole request path ran interpreted, a `sub sp, sp, x9` that silently
allocated nothing.

None of those would have been found by writing another test for a feature
that exists. The lesson is kept as a decision because it is a rule about
how to build the rest: build the thing that uses the language end to end,
and keep it in the repository where it runs on every change.

**D70. The stack pointer is not a register like the others.** In the
shifted-register form of `add` and `sub` on arm64, register 31 is the
*zero* register where the stack pointer would be -- so `sub sp, sp, x9`
allocates nothing and `add x9, sp, x10` reads zero. The assembler now
refuses that encoding instead of emitting it, because the alternative is
an afternoon spent in a debugger on generated code. A large frame is
adjusted with two shifted immediates, which reach sixteen megabytes
between them and name the stack pointer legally.

**D71. A machine is a type, and its methods are what the table means.**
`machine Door { state Closed { on Interact -> Opening } ... }` declares a
type whose value is which state it is in. From the table the compiler
generates one method per event, one predicate per state, and the `enter`
and `exit` blocks as methods of their own -- so `door.Interact()` and
`door.IsOpen()` resolve with no special case above the member table, and a
machine from another module works for the same reason a class does.

A machine is a *handle*, like a class: an event changes which state it is
in, and a method that changes its object has to be given the object rather
than a copy. `always` is resolved when the table is lowered rather than at
run time -- entering a state that always leads on enters both, and every
state on the way runs its `enter`. A cycle of `always` stops at the state
it has already been through.

What is not here: `on Interact after 3s`, which needs a clock the language
does not have yet, and a `wait` inside `enter`, which would make a
transition suspend. Both are in the syntax and neither is lowered; the
spec marks the section a draft and this is the part that is still draft.

**D72. A synthesized member needs an identity of its own.** A machine is
one declaration and many functions, so (file, item) does not tell them
apart -- and a call that cannot tell two methods apart calls whichever came
first, which is what happened. Members and IR functions carry a `slot`
beside the item now, and the linker matches on all three.


**D73. `soa []T` is a layout, not a type.** The same record, the same
fields and the same code reading them; what changes is where the storage
puts them. A pass that touches one field of a wide record reads every
byte of every cache line it pulls in instead of one field's worth of each,
and a game spends most of its frame in exactly that kind of pass.

Each field gets an array of its own. Their sizes are summed in declaration
order, which makes the start of a field's array the sizes before it times
the number of elements -- and *that* is what keeps an address cheap: one
multiply, not one per field. The count each array really holds is rounded
up to sixteen, which is what makes the multiplier a compile-time constant
and leaves every array sixteen-byte aligned for a vector load later.

What this does not do is make a function work on both layouts: the layout
is in the type, so `fun Sum(bodies: soa []Body)` takes the one it was
written for. Polymorphism over layout needs a generic parameter that
ranges over storage, which is not here yet.

**D74. A call takes the type arguments it writes.** Inference reads the
arguments, so a signature that mentions its parameter only in its *result*
-- `SoaNew<Particle>(count)`, whose only argument is a number -- has
nothing to be inferred from. The explicit arguments are matched against
the parameters the signature mentions, in the order they appear, and
inference fills in the rest.

Found by what it broke rather than by thinking: before this, the call's
type stayed `soa []T` and every field offset was computed against a type
parameter, so every field of every element landed on top of the others and
a sum of four elements came out as the last value written four times.

**D75. Arithmetic the compiler writes for itself is not checked.** An
address is a multiply and two adds, and a trap in one of them would name a
line the program never wrote. The instruction carries `is_unchecked`, and
a tier emits it without the overflow check and the reference tier wraps
rather than traps.

The second reason is the one that mattered more: an instruction that
cannot trap can be moved out of a loop. Hoisting a *checked* multiply out
of a loop that never runs would trap where the program would not have, so
the flag is what makes the arithmetic below hoistable at all.

**D76. An element address is expanded into the arithmetic it is.**
`index.addr` and `soa.addr` exist so that lowering needs no layout. Once
the layout is known, keeping them opaque costs more than it saves: the
base of a field's array and the length it is checked against are the same
every time round the loop, and nothing can hoist what it cannot see
inside. A pass with the layout in hand expands `soa.addr` into unchecked
arithmetic, grouped as `(base + array) + index * size` so that the half
about the field hoists out and the half about the element stays.

What stays behind is `index.check`, which is the part that is about *this*
element. It produces no value, so the pass that removes dead code deleted
it until it was marked as doing something -- a bounds check that
disappears is the kind of bug a benchmark reports as an improvement.

`index.addr` is left alone: the reference tier reads an array inside a
slot as a path rather than an address, so arithmetic is not something it
could stand in for.

**D77. Two passes for the loops this language is for.** Common
subexpressions, one copy per block: reading a field, adding to it and
writing it back asks for the same address twice. And loop invariants,
hoisted into the one block that runs before the loop: the base of a
slice's storage, its length, the start of a field's array.

Together they took a million-entity integration from 107 ms to 29, which
is more than every other pass has been worth put together -- the passes
that were there folded constants and promoted slots, and neither does
anything for a loop over a buffer.

A loop is found by a jump backwards in reverse postorder *and* dominance,
not by the jump alone. A resumed task is entered by a jump table that
lands in the middle of its loops, so "the only way in from outside" is not
the same question as "always runs before" -- which the task tests said in
the form of a value used where it was not defined.

**D78. A generic type is instantiated where it is built.** `Box<int>(7)`
binds the declaration's parameters from what the call wrote, or from the
arguments when it wrote nothing, and the result is the instance rather
than the declaration. A field of it then reads as `int`, a layout can be
taken of it, and two instantiations are two layouts.

Members live on the declaration, so an instance's member table is the
declaration's with its parameters bound. That is built once, in the
resolution that answers `MembersOf`, rather than at each of the dozen
readers -- a layout, a lowering, an interpreter, a checker -- because each
one getting it right separately is a dozen chances to get it wrong. The
one that was wrong was the layout: an instance was not a named type to it,
so every field of a `Box<int>` sat at offset zero and reading one gave
another.

A method of a generic type is a recipe, like a generic function: one copy
per instantiation, keyed by the mangled name `Box<s32>.Get`. The
substitution comes from the receiver rather than from inference, which
makes it the simpler half -- by the time a method is called, what its
type stands for is already decided. And whether a method writes through
its receiver follows the declaration: an instance of a class is a handle
exactly when the class is.

**D79. A generic is copied where it is used, with the names of where it
was written.** The module a generic lives in cannot know what its
importers will ask for, so the copy is made in the module that asks. What
makes that work is that the copy borrows the defining file's view for as
long as it takes to lower: its syntax, its member tables, the types its
checker recorded. `Store<T>`'s body resolves `SoaNew` in the file that
wrote it, not in the one that called it.

Lowering therefore has a *current file* rather than one file, and
everything it looks a name up in is keyed by that file. The bug that said
so: a table of this module's declarations was searched by syntax id while
another file's syntax was in force, and a constructor call came out as a
call to an unrelated constant that happened to sit at the same index.

A specialization has no source identity. It is reached by the name it was
mangled with -- `Store<Position>.Add` -- because two instantiations of one
generic would otherwise claim the same (file, declaration) pair, and a
linker that resolves by that pair binds a call to whichever copy it finds
first. Which is a call into another type's layout.

**D80. The reference tier reads and writes aggregates through pointers.**
A class holding a slice has a sixteen-byte field, so writing it means
writing a pointer and a length at the offsets the layout gives. The
interpreter had no way to do that at all: it switched on the *value* and
every aggregate fell through to a trap. It now walks the layout, which is
the same answer the compiled tiers get from the same store -- and a slice
is spelled out rather than looked up, because neither of its fields is a
member anyone declared.

What is still not possible is putting a *local* array into a heap object
in that tier: a local lives in a frame slot, which the interpreter holds as
a value rather than at an address, so there is no pointer to write. The
storage an ECS keeps is allocated, so this has not come up outside a test
that was written wrong.

**D81. A load is reused until something writes.** Within a block, reading
the same address twice with nothing stored or called in between gives the
same answer, so the second read is dropped. A loop that reads a field of
its object for every use pays for it once instead.

What this does *not* do is move a load out of a loop. That needs to know
that nothing the loop writes lands where the load reads, which is alias
analysis -- and without it the base of a store's row and the length it is
checked against are re-read a million times. It is the largest single
thing between this and the layout difference showing up as more than a few
percent, and it waits for the wave that brings the analysis.
