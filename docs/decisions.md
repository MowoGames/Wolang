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

**D21. `extend T` and `extend T as Ability` replace `impl`.** The target comes
first either way, so the declaration reads as a sentence. `impl Ability for
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

**D35d. Ability conformance is declared, not structural.** `extend T as Ability`
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

**D82. A system declares its access, and the schedule follows.** `mut T`
writes, `T` reads, `with T` and `without T` filter -- which is reading
whether an entity has the component. From the declarations alone the
compiler lays the systems out in stages: everything in a stage may run at
once, because nothing in it writes what another touches. Two that would
share a stage and conflict are rejected rather than ordered arbitrarily
(`E0127`), because the arbitrary order is the bug a scheduler like this
exists to prevent.

The access lives in the *syntax*, not in the type: `Query<mut Velocity,
with Falling>` resolves to one query type over the components it names, and
the modifiers are read off the declaration by the pass that schedules. One
exception travels in the type -- whether the component the loop binds is
written -- because `foreach v in q` has to know whether `v` may be assigned
to, and that is a checking question rather than a scheduling one.

Why in the compiler rather than in a library: the schedule is a property of
every declaration at once, and nothing a library can see spans them. The
graph is a module-level query, so two systems in different files are
scheduled against each other and an edit to one rebuilds only what depends
on the answer.

What is not here: a `Query` value built from a world, and therefore running
a system at all. The storage is a library (`game.ecs`), the schedule is the
compiler's, and the join between them -- which store a query reads, and how
a stage is handed to a thread -- is the next piece. Running the stages in
parallel needs threads in the runtime, which do not exist yet.

**D83. The programs are generated, and the tiers are compared.** Every
hand-written codegen test runs one program through all three tiers and
compares; the fuzzer writes the programs. A seed names one program for
ever, so a disagreement can be looked at again -- which is the whole
reason the generator uses its own xorshift rather than anything from the
standard library, whose engines are free to differ between
implementations.

It earned its keep in the first two hundred programs, with two
miscompiles in passes that had been there for three waves:

- **Constant folding was unsigned.** `-2 / -10` folded to one instead of
  zero, and every signed comparison on two constants was wrong the same
  way. Overflow was folded into its wrapped answer rather than left for
  the trap it is at run time.
- **An element address was treated as pure**, so loop-invariant hoisting
  moved one out of an `if` inside a loop and into the loop's preheader.
  The program returned nine before and trapped after: the bounds check is
  part of what the instruction does, and what can trap cannot be moved to
  a path that would not have reached it.

What the generator does *not* do is interesting too. It writes loops whose
counter it keeps out of the body, because a body that may assign to the
counter is a body that may never end -- and a compiled tier has no step
limit to stop it with. A program the reference tier could not finish is
skipped rather than compared: it says nothing about the other two.

Traps are compared rather than avoided. Division by zero, overflow and an
index past the end are defined outcomes, so a tier that wraps where
another traps is exactly the bug this is looking for.

**D84. A derived object is a base object at the same address.** A class's
fields are the fields it inherits, root first, and then its own. So a
handle to a `Cube` is a handle to a `Square` is a handle to a `Shape`, and
an upcast is nothing at run time -- the IR says so with a cast because
every type in it is exact, and the cast emits no instruction.

Everything that asks what a class has now asks along the chain: the
constructor takes the inherited fields first, a method reads a field its
base declared without qualification, and a field access through a handle
reaches an inherited member. Each of those was a separate place that only
knew about a type's own members, and each one was a separate bug report
waiting -- `Cube has no member named tag` for a field `Square` declared.

**D85. The number in the object is the class's declaration.** A virtual
call reads a token out of the object and uses it to find the body. The
token is `(file << 32) | item` -- which declaration this class is -- and
not an index into anything a module owns, because an object created in one
module is dispatched in another and the two have to agree.

Slots are numbered from the root: the root's virtual methods in
declaration order, then each derived class's new ones, with an `override`
taking the slot of what it overrides. That makes one number mean one
method everywhere the class is visible, including a module that only sees
the declaration.

The tiers differ only in how they get from the token to the body. The
reference tier looks through the class tables of the modules it holds. The
compiled tier asks the runtime, which the compiler told where each method
ended up once the code was placed -- a C call per virtual call, which is
the price of the number meaning the same thing in both tiers. Inlining
that lookup is the obvious next step, and it is why the registry is a flat
array rather than a hash map.

A class gets the header only when something in its chain is virtual, so a
`class` with no virtual methods costs exactly what it did before.

**D86. An ability value is a pair: what it points at, and which class that
is.** Two words, like a slice, and for the same reason -- neither half is
a member anyone declared, and a call on it needs both. The method is named
by the *ability's* declaration, so the body is found without the call site
knowing anything about what implements it.

A class hands over its handle, because that is already an address. A
value type is **copied onto the heap**: an ability value is the one thing in
this language that outlives the expression that made it -- a list of
shapes is the point of having it -- and the reference tier's locals have
no address that survives a call, so a borrow of a local could not work in
every tier. Nothing frees the copy yet; that waits for destructors, and it
is the next thing this needs.

A method of a value type takes `self` as a value, and an ability call has
only an address, so the table gets a one-line function that loads the
value and calls the method. Every vtable over value types needs that
thunk; a class needs none.

The same `wolang_class_lookup` answers both this and a `virtual` call,
because both ask the same question: this object, this method declaration,
which body? That is what made the selector worth having instead of a slot
number -- a class's own virtuals and every ability it implements share one
table without agreeing about an ordering.

**D87. `deinit` is what a type does when a value of it goes away, and that
makes the type move rather than copy.** The two halves are one feature: a
type that frees something cannot be in two places, so handing it to
something else is a *move* and the name it was in no longer holds it. A
type without a `deinit` is unaffected -- it copies, exactly as before --
which is what keeps the rule opt-in rather than a tax on every class.

A scope exit runs the destructor and then frees the object, through the
same machinery `defer` uses, which is why `defer` was built first (D61).
Whether it runs is a **flag**: set where the binding is initialized,
cleared where it is moved, tested on the way out. The flag rather than the
analysis, because lowering would otherwise need to know what the checker
worked out about every path -- and the flag folds away wherever the passes
can see both ends.

What counts as a move is the question the checker already answers for
`String`: a use is a borrow when it is read *through* -- `b.size`,
`b.Method()`, assigning to `b.field` -- and a move when the value itself is
handed over. Lowering follows the same rule, so the two cannot drift.

A parameter that was handed ownership destroys it on the way out. That is
what makes a move a transfer rather than a leak, and it is why a function
taking an owning type is written as taking the type: a borrow is written
`&T`.

**D88. A module-level `var` has storage, and a module-level declaration
says its type.** A `const` becomes a function the inliner folds, which is
right for something immutable; a `var` can be written, and a function that
recomputes the initial value is not storage. Before this, `counter =
counter + 1` compiled to nothing at all and nothing said so.

The storage belongs to the *runtime*, not to either tier, for the reason
the class tables do: a program may be half interpreted and half compiled,
and a counter written by one half has to be the counter read by the other.
The token is the declaration, so both tiers ask for the same storage
without anything handing out addresses. A global starts as its type's zero
and takes the source's value in the file's init function, which runs
before anything else -- and the storage is reset at the start of every
run, so that running a program twice is running it twice rather than
continuing it.

The type has to be written (`E0130`). Not a style rule: what a name means
to another file is worked out from signatures alone, and inferring this one
from its initializer would make a signature depend on a body -- which is
the thing the incremental design is built not to do.

**D89. An enum is a tag and room for the largest payload.** A variant that
carries nothing is the tag alone, which is what an enum was before this; a
variant that carries values puts them beside the tag, at one offset shared
by every variant. That is what lets a `match` read them from one place
once it knows which variant it is looking at.

The payload's *type* is the instruction's rather than the declaration's.
Nothing else would do: which variant it is is exactly what a `match` is
for, so the field table cannot name a type for the payload and the
instructions that build and read it carry the variant instead.

A variant that carries values is typed as a *function* from them to the
enum, which is how `AiState.Chase(target, speed)` ends up being an
ordinary call at the use site -- and how the checker knows what a pattern
binding holds.

**D90. A `T!` is which it is, and the value when it is a value.** The tag
is zero for a value and otherwise says which error, named by the error
tag's own symbol -- so the construction and the test agree without either
side knowing about the other, including across files.

An error tag *is* that number as a value. `return Corrupt` coerces it into
the pair; `match` compares against it. The alternative -- an undefined
value standing for the error -- is what the first version did, and an
undefined value is a trap in two of the three tiers.

`?` is an early return: if the tag is not zero, the same tag goes back as
this function's failure, with no value beside it. That is the whole point
of the operator -- the alternative is a `match` at every call -- and it is
why a signature's error set is the union of what its body propagates.

**D91. The collections are written in Wolang, and a map takes its hashing
as values.** `List<T>` and `Map<K, V>` are classes in `std`, over
`ArrayNew` and `Free`, with a `deinit` that gives the storage back. That
is the return on generics across modules and destructors: before either,
a growable container could not be written in the language at all.

A map compares and hashes through `fun(K) -> int` and `fun(K, K) -> bool`
rather than through an ability. An ability would need a `Self` type to say
"equal to another one of me", which the language does not have yet -- and
the shape this leads to is honest rather than apologetic: `MapNew(IntHash,
IntEquals)` says what it does, and a map keyed by something unusual needs
no declaration anywhere.

Two things the library ran into first, and both are the language being
right rather than in the way. A hash cannot be a 32-bit multiply that
throws the high bits away, because throwing them away *is* the overflow
trap -- so it is done in sixty-four bits and masked back. And a function
value in a field had no way into memory in the reference tier until now:
it is a module and an index there, so what goes into the field is the
token a call turns back into the value.

**D92. The platform's numbers stay in the platform layer.** `open` is a C
call the language can make, but its flags are not the same number on two
systems -- `O_CREAT` is 64 on Linux and 512 on Darwin. A library written
in Wolang cannot say both, and the version that guessed opened a file with
no write permission on one of them, which is how the first `std.fs` test
failed. So the flags live in C, behind `wolang_open_read` and
`wolang_open_write`, and everything above them is Wolang.

The clocks are two functions rather than one for the same reason the
language has two words for mutability: they answer different questions. A
monotonic clock never goes backwards and is what "how long did that take"
wants; a wall clock does go backwards when the system is corrected and is
what "when was that" wants. One function would make the wrong one the
default half the time.

**D93. `xs[a..b]` is a view, and a slice of a slice is arithmetic.** The
pointer is where element `a` is and the length is `b - a`, so no copy
happens and a view of part of a buffer costs two words. Both ends may be
the length itself -- an empty tail is a useful thing to ask for -- so the
bound each is checked against is one past it.

**D94. A slot is not an address.** `&x` on a whole local needs the address
of the frame slot, which the reference tier does not have: its places are
paths, not numbers. So there is an instruction for it -- the reference
tier answers with the slot, a tier with real frames answers with the
address -- and it is emitted *only* where an address is wanted.

The first version emitted it for every read and write of a local, which
made every slot's address escape and stopped slot promotion dead: a loop
counter went back to making a round trip through the frame, at a third of
the speed. The lesson is the one the register allocator taught in wave 8:
a change that makes something expressible has to be confined to where it
is needed, because the optimizer's questions are about what *cannot*
happen.

**D95. A server asks which connections are ready.** The blocking loop held
one client at a time: `read` waits, and everything else waits with it. The
polled loop asks the kernel which of the descriptors it is holding has
something to say, so one thread serves as many connections as the system
will give descriptors for -- and an idle connection costs one entry in a
kernel table rather than a thread.

Measured on this machine, against the same handler: 500 concurrent
clients, 20,000 requests, **43,455 requests a second and no errors**. The
blocking loop cannot be measured the same way at all -- with 500 clients
it serves them one after another.

The mechanism is the platform's and the loop is Wolang. kqueue here and
epoll there differ in structure size and field order, which is not
something a library written in this language can spell, so `poll.h` is
three calls and `http.poll` is a class over them. The same division as
`std.fs` (D92), for the same reason.

One request per ready connection per turn, rather than draining each: a
client that keeps asking would otherwise hold the loop for as long as it
likes. The listener *is* drained, because it reports readable once however
many connections arrived.

**D96. A type says how to walk itself with three names.** `for x in thing`
asks `thing.Iterate()` for something with `Next` and `Value`, and that is
the whole protocol. An ability would be the obvious home for it, but an ability
that said "what I walk over" needs an associated type the language does
not have -- and three names need no new feature at all.

The loop is built in lowering rather than desugared in the parser, so a
diagnostic points at the loop the program wrote. `E0131` is what a `for`
over something with none of those names reports, and it says which three
to add.

Walking borrows. `for key in map` asks the map how to be walked and reads
what comes back; it does not take what the name holds, so the map is
still there afterwards -- which the move analysis had to be told, because
an owning value handed to anything is otherwise a move (D87).

**D97. Threads, a lock and one atomic, and the reference tier runs a
spawned body on the spot.** The smallest set that makes "do these at the
same time" expressible: `Spawn`, `Join`, a mutex by handle, and an atomic
add. Everything above it -- a pool, a channel, a work queue -- is Wolang
over those, which is where it belongs.

A thread in the reference tier would be a second interpreter: its values
are not addresses and its frames are its own. So `Spawn` there runs the
body immediately, on the calling thread, and hands back a handle that is
already finished. A program whose answer does not depend on interleaving
gives the same answer in both tiers -- which is the only kind of program
the differential check can compare -- and one that *does* depend on it has
a bug worth finding rather than hiding.

A function value crosses the C boundary as what it is, an address. That
is what every threading API in C takes for a callback, and the rule that
kept it out (`E0802`) was about layouts this compiler decides
unilaterally, which a function pointer is not.

**D98. A third tier: the same IR, through LLVM, into a binary.** The
interpreter is the reference and the dev tier is what a developer waits
for; neither is what a program ships as. `wolang build` turns the
optimized IR into an object file and links it with the runtime archive,
and the result needs no compiler to run.

Through LLVM's C API rather than its C++ one, because this project builds
without RTTI. The translation is deliberately literal: one LLVM
instruction per IR instruction, a block parameter as a phi node -- the
form the IR was given so that this would be one line -- and every
aggregate as the address of storage made once in the entry block. Every
value is an integer or a float of its own width, and a pointer is an
`i64`: the optimizer has already turned element addresses into ordinary
arithmetic, so a value that is added to has to be a number, and it
becomes a pointer at the one place that reads through it.

Three things the dev tier does for free had to be said out loud here. A
frame is zeroed, because a program that writes two bytes of a socket
address and hands all sixteen to C needs the other fourteen to be zero. A
trap reports and exits, because a binary has nobody to report an outcome
to. And the class tables are registered at startup by a function the
backend writes, because nothing is there to register them as it compiles.

What the backend cannot translate is named rather than guessed at: the
driver prints every function left out, since a binary missing a body is
incomplete and which body is what makes that worth fixing.

**D99. A block gives nothing back by itself.** A function that declares a
result returns it with `return`, or is written with `=> expr`. A block
whose last statement happens to be an expression is not a return.

This was a hole rather than a decision: such a function compiled, and
returned whatever the register held. `E0132` is the diagnostic, and
`loop { }` with no `break` in it is not an error because it never reaches
the end at all.

**D100. A world the runtime holds, and three intrinsics to reach it.**
`component`, `Query` and `system` with a schedule have existed since wave
11 with nowhere to put the data: a system was checked, laid out in a
stage, and could not run. The storage is one dense array per component
token -- the same `(file << 32) | item` number a class carries -- with an
entity index beside it, held by the runtime.

In the runtime rather than in Wolang because a world is a map from a
*type* to an array, and the language cannot spell a map whose keys are
types. A token can, so the library asks for the array it means. Three
things the compiler had to answer for that to be writable in Wolang:
`TokenOf<T>()` (the number, carried in a `Token<T>` so that the result
type says which), `SizeOf(value)` (how many bytes a value takes, which
only a tier knows -- hence `type.size`), and `ViewFromRaw<T>` (a view of
memory as values, the way `BytesFromRaw` is one of bytes).

Three real bugs fell out of writing it, each silent before:
`E0133` -- a type parameter that appears nowhere in a signature can never
be said by a call, and such a call lowered to a body that was not there;
a generic body calling another generic *of its own file* looked for it in
the calling file and found a declaration instead; and `return value`
where `T?` was declared emitted no wrap at all, because the checker
records the type the *context* wanted and lowering believed it. The last
one is why `Coerce` now asks the value what it is rather than asking the
expression.

**D101. A system is a function of the world, and a stage is a set of them
run at once.** `system S(q: Query<mut Position, Velocity>)` compiles to
`void S(uint64_t world, float delta)`: the query is not a parameter at run
time but a declaration of access, so what the body gets is a walk over
the entities of that world which have what it named. `RunSystems(world,
delta)` is the schedule the compiler worked out -- stage by stage, with
the runtime starting a thread per system in a stage, which is what the
stage *means* (D82).

`mut T` in a query resolves to a mutable reference to the component. That
is what makes the loop's binding assignable -- a system that writes says
so once, in its signature -- and what the loop binds is where the
component *is*, not a copy of it.

What `foreach` costs was worth three rounds. A call per entity to ask
where the component is, with a lock and two lookups inside it, ran 100k
entities in 6.5 ms per frame. The walk goes along the first component's
column by index rather than along the smallest; the filter's columns are
resolved once per query instead of per entity; and the addresses are
handed over as one array, so the loop itself is arithmetic. Same answer,
1.2 ms. What is left is one lookup per entity per extra component the
filter names, and an array of pointers built per query -- both of which
archetype storage would remove, and neither of which is worth the
complexity before something needs it.

**D102. `Owned<T>` and `Shared<T>` are what destructors were for.** A
`class` value is a handle to a heap object and nothing frees one: there is
no collector, so the language has to let a program say when something goes
away. `Owned<T>` is sole ownership -- moving it hands the value over, and
its destructor frees it. `Shared<T>` is a count beside the value, with
`Clone` to take another handle and the last one out freeing it. Both are
ordinary Wolang in `std.own`, over `deinit`, moves and the three
intrinsics of D100.

No implicit copy for either, and that falls out rather than being decided:
a type with a destructor is moved rather than copied (D87), so taking
another handle is something the program says.

Two compiler bugs came out of writing them. A type could not construct one
of itself -- `Holder<T>(value)` inside `class Holder<T>` bound `T` to the
`T` in scope, and a check that read "bound to itself" as "nothing bound
it" rejected it. And a runtime function declared both by a library
(`extern "C" fun wolang_object_free(at: *mut u8)`) and by lowering (where
a handle is a word) kept one declaration and two opinions about its
parameter; a call now says what the surviving declaration says.

**D103. A `comptime` call with constant arguments is a constant.** One
with no arguments was already evaluated inside the compiler, to check that
it could be; one *with* arguments needed the call site, which is this. The
fold happens on the IR, before the passes: before them so that the
reference tier sees the same program, and before inlining in particular,
which would otherwise take the call apart and leave nothing to evaluate.
`Fib(20) + 1` in a body is one `const.int 6766` in every tier.

It runs again after the passes, for the calls whose arguments only became
constant once propagation had been through. A trap is reported the way it
is for one with no arguments (`E0901`), because a `comptime` call that
divides by zero is the program telling the compiler something is wrong.

**D104. `trait` is spelled `ability`.** The concept is unchanged -- a set
of requirements a type can satisfy, with static dispatch through a bound
and dynamic dispatch through a value (D86) -- and the word is the one a
gameplay programmer already uses for "what this thing can do".
`ability Drawable { ... }` and `extend Circle as Drawable { ... }` read as
one sentence, which `trait` never quite did.

The old spelling stays reserved so that a file written before the rename
says `trait is now spelled ability` rather than failing to parse
somewhere further along.

**D105. The bounds check a loop already made is removed.** `while i <
bodies.len { bodies[i].x }` compared the same two values twice: once
because the program said so, and once because reading an element must not
be allowed to read anything else. The second is removed where the first
is the *same* comparison against the same length, on the edge where it was
true -- the one shape that needs no proof about arithmetic.

Two things had to come first. A field read of something built right here
is forwarded (`extract 1` of a `soa.new` is the count it was given), which
is what makes the length of `SoaNew<Body>(kCount)` the constant `kCount`
again; and facts are inherited only along an edge into a block with one
way in, which is what makes one pass over the blocks sound -- a loop
header has two ways in, so it starts knowing nothing, and its body is
told what the branch said.

The 1M-entity integration went from 43.3 ms to 25.1 ms, and the one-field
pass from 44.7 to 22.3. What is left in the loop is a multiply, four adds
and the loads -- and the AoS variant now wins the integration benchmark
(21.1 ms against 25.1), which is the honest answer for a pass that
touches four fields of six: `soa` is for the ones that touch one or two,
where it is ahead by a quarter.

**D106. A closure is a function written where it is used, and it takes
nothing with it.** `|n| n + 1` becomes a function of the module with the
body the expression wrote, and its value is that function's address --
which is what a `fun(...)` value already is in every tier. Nothing is
allocated, nothing is boxed, and a closure crosses the C boundary like
any other function pointer.

The parameter types come from the type the closure is being given to:
`Apply(|n| n + 1, 10)` where `Apply` takes a `fun(int) -> int` needs no
annotation, and one written with no such context says so (`E0134`).

Capturing is a diagnostic rather than a feature (`E0135`), and the message
names the local and what to do instead. The reason is the same one behind
the rest of the language: a captured local has to live somewhere after the
frame goes away, and that somewhere is either an allocation the program
did not ask for or a lifetime nobody checked. A type with the state and a
method on it says both, and costs nothing that is not written down.

What a capturing closure *would* cost is worth writing down, because it is
the next decision here and not a small one: a function value would become
a pair -- code and environment -- which changes its layout, the indirect
call in three back ends, what crosses to C, and who frees the
environment. Said plainly rather than left as "later".

**D107. `WrappingAdd`, `WrappingSub` and `WrappingMul`.** Everything else
in this language traps on overflow, which is right for a program counting
damage and wrong for one hashing a key or stepping a generator: those want
the low bits of a wrapped result. The three have no body -- what they
stand for is the instruction the compiler already writes for its own
address arithmetic -- and they are what made `std.random` and a decent
hash writable in Wolang rather than in C.

Two things fell out of writing `std.sort` over them. A closure passed to a
generic now gets its parameter types from the parameter with the type
variables filled in by the arguments before it -- `Sort(items, |a, b| a <
b)` binds `T` from `items` and the closure from `T`. And a generic that
calls itself records that its own parameter stands for itself: inference
has nothing to do there, recording it as "no arguments" left the call with
no instantiation to find, and a recursive `Quicksort` lowered to an
`undef`.

**D108. An import used only by an enum's payload counts as used.** `Array(items: List<Json>)`
is the only mention of `List` in a module, and the unused-import warning
did not see it: variant payload types are resolved where the enum is
checked, and that walk did not record the names it used.

**D109. A method on a reference is the method of what it refers to.**
Without it an owning value could not be lent at all: handing a `Builder`
to a helper moves it (D87), so the way to let the helper write into one is
`&mut Builder` -- and a reference with no members made that unusable. A
reference to a class holds the handle, so the handle is loaded out of it;
a reference to a value type already is the address a method takes. `&x`
borrows rather than moves, and a reference is a pointer in the IR, which
is what keeps a `&mut Builder` parameter and the `&mut builder` passed to
it the same type.

**D110. `+` joins two strings.** Interpolation covers most of it --
`"{a}{b}"` is the same call -- but a program building a string from pieces
it has in hand reads better with `+`, and every user tries it first. A
string plus a number is still an error, with a message that says which
two things `+` takes.

**D111. Five places where lowering believed the checker's recorded type.**
The checker records the type the *context* wanted at an expression. That
is what makes `Accepts(T?, T)` work, and lowering read it as "what this
expression is" in five places -- each of which produced a value of the
wrong shape with no diagnostic anywhere:

- a call whose result is used where a `T?` is expected was typed `T?`
  while the function returned a `T`;
- a field read in the same position was typed `T?`, so an enum's bytes
  were read as an optional's;
- `return value` where `T?` was declared emitted no wrap at all (D100).

The rule now: an instruction's type is what *produces* it -- the callee's
result, the member's type, the value's own type -- and the conversion to
what the context wanted is a `Coerce` like every other.

Three more bugs came out of the same work, each found by writing
`std.json` rather than by a test expecting them: the drop flags of one
function were still in scope while the next was lowered, so a flag's
instruction id meant something else entirely; `local.addr` in the
reference tier answered with a frame-relative slot, which meant a
different slot in the callee it was passed to; and storing or loading a
tagged enum through a pointer used an aggregate's field offsets, where an
enum's payload is at the offsets *that variant* gave it.

**D112. A request is views, and a router is a list of patterns.** What a
server needs beyond a method and a path -- headers, a query string, a
body, path parameters, a status and content type -- is now there, and all
of it costs no allocation: a request's headers are a fixed run of
name/value views into the connection's buffer, and so are a response's.

Fixed rather than grown, and that is the decision. A `List` owns its
storage, so a request holding one would be a value that has to be
destroyed -- while a request is built by a parser, handed to a router,
rebuilt with its parameters, given to a handler and dropped at the end of
a turn. Every one of those steps would be a question about who frees it,
and two of them got the answer wrong before this. A fixed run has no
answer to give: twenty-four headers and eight parameters, and a request
with more keeps the first ones.

The router matches segment by segment -- `:name` takes a segment, `*`
takes the rest -- in the order the routes were added. No tree and no
regular expressions: an API has tens of routes, and the walk is what makes
`:id` cost a pair in an array. A path that matches with the wrong method
is `405` rather than `404`, which is the difference between "no such
thing" and "not like that".

A write through a mutable reference is the reference's business rather
than the binding's: `fun Matches(.., into: &mut Request)` says the thing
referred to may be written, and the name holding the reference is not
what is being assigned.

**D113. Freeing storage an `arena` handed out is not a free.** A
destructor that runs inside an `arena` block frees what it owns -- and
inside a block, what it owns came from the arena, which reclaims it when
the block ends. `free` on that is a pointer the allocator never gave
anyone, and it aborted the process on the first request a server served
with a string builder in it.

So `wolang_object_free` asks the arena first. Not a flag on the pointer or
a header before it: the arena knows its chunks, the check is a walk over a
handful of ranges, and it happens only on a free. The arena stack is also
per thread now, which it had to be the moment a stage of systems ran in
threads (D97).

**D114. An optional expectation is not an arithmetic one.** `-total` where
an `int?` is wanted is a negation of an `int`; passing the optional down
made the operand an optional and then reported `-` as needing a number.
The expectation is stripped of its optional before it reaches an
arithmetic operand -- which is the checking-side twin of D111, where the
same confusion made lowering emit the operation at the wrong type.

**D115. Arguments are given to the runtime, and an address can be a
number.** A program's arguments come from whatever started it -- `main`'s
`argv` in a binary, whatever followed `--` when the driver ran it -- so
the entry point hands them over and everything else asks the runtime.
`std.env` is that, plus the environment: `Lookup` answers with an optional
because a variable set to nothing and one that is not there mean different
things.

`p as u64` is now allowed in `unsafe` code. Comparing an address with
zero, hashing one, handing one to C as a word: all three were
unspellable, and the promise that a number is an address is exactly what
`unsafe` records.

**D116. `wolang doc` writes what a package offers, from the source.** A
signature is taken from the program's own spelling rather than rebuilt
from the syntax tree: a default value, a `where` clause and the order of
the effects all say something, and a renderer that rebuilt them would be
a second grammar to keep in step. Markdown because it reads as text and
renders where the code lives.

Only what another module can name: a private helper is an implementation
detail by definition, and `docs/api/*.md` is what the four libraries
offer today -- regenerated with one command rather than written by hand.

**D117. The editor's end is a protocol over the queries that already
exist.** `wolang lsp` speaks the Language Server Protocol on stdin and
answers with what the compiler already knows: the diagnostics `check`
produces, the types the checker recorded, the declaration the resolution
points at, the text the formatter writes. Nothing in it is a second front
end, which is the only way an editor and a build can agree about a file.

Whole-document sync rather than incremental edits: the compiler holds the
text, and an incremental edit would be a second copy of it to keep in
step. Its own small JSON reader rather than `std.json` -- that one is
Wolang, and this is the compiler talking over a pipe before any Wolang has
run.

What it answers today: diagnostics on open and on change, hover with the
type and the doc comment, go to definition for anything the file
resolution knows, the file's symbols, formatting, and completion from
file scope. A completion that knew about locals needs a scope per
position, which the checker does not record yet -- said here rather than
left as a surprise.

**D118. The IR carries where every instruction came from.** A side table
parallel to the instructions, not a field: an `Inst` is a fixed size on
purpose and a span is three words. Every pass that makes an instruction
keeps the table in step, and the inliner copies the *callee's* span --
an inlined body is still the callee's code, and a trap in it should name
the callee's line.

Three things come out of it. A trap says
`app.wo:4:10 (@main:%55): index is outside a length of 4` in both tiers
instead of naming an instruction number. `wolang build` emits DWARF, so a
debugger stops on a line of Wolang, steps statement by statement, and
names Wolang frames in a backtrace -- which needed `dsymutil` as well,
because on this platform the line table stays in the object file and the
binary holds only a map into it. And a profiler can attribute a sample to
a function, which is the next thing this makes possible.

`--no-debug` leaves it all out, which is a fine thing for a release build
to ask for.

**D119. A method may have type parameters of its own.** `fun Map<T>(f:
fun(int) -> T) -> T` on a plain `class Box` is a recipe like any other
generic: one copy per set of arguments, named with them so that
`Box.Map<int>` and `Box.Map<f32>` are two functions. It type-checked
before this and lowered to a call with no body behind it, which is the
worst kind of gap -- silent.

Two things in the checker had to give way. Half an answer is enough for a
closure: `fun(int) -> T` says what the closure *takes* while its result is
what is being inferred, so the hint is passed down and the result is
taken from the body. And an expectation that still mentions a type
parameter is a *pattern* rather than a type -- adapting against it
reported the very answer that binds the parameter as a mismatch.

**D120. A tag-only enum round-trips through memory as one field.** The
interpreter holds an enum with no payload as a one-field aggregate --
which is what `match` reads with `extract 0` -- and memory holds it as a
number. A global one was written as nothing and read back as a bare
number, so `match` on it trapped in the interpreter while both compiled
tiers answered: the same producer-and-consumer disagreement as D111, one
tier further down. Found by `std.log`, whose level is a global enum.

**D121. The libraries a real program reaches for.** `std.fmt` (columns,
bases, decimals, sizes, durations), `std.path` (textual, no filesystem),
`std.utf8` (code points, kept out of `std.text` because most programs
never need them), `std.hash` (FNV-1a, with the warning that it is not
keyed), `std.encoding` (base64, hex, URL), `std.net` (a socket floor of
its own, so a program can speak something other than HTTP), `std.process`
(exit, pid, a shell command whose string is the caller's
responsibility), `std.log` (levels, a global threshold, standard error)
and `std.io`'s other half (stdin by line, stderr).

`std.time` grew a calendar: civil-from-days both ways, ISO 8601, and the
date format HTTP wants. No time zones -- they are a database that changes
four times a year, and a program that needs one should read that database
rather than have this guess.

Writing them found two miscompilations. An inlined `WrappingAdd` lost its
"do not trap" flag, because the inliner copied an instruction field by
field and that field was not in the list -- so a hash overflowed where it
is supposed to wrap.

**D122. A request is read whole before a handler sees it.** The old loop
parsed whatever one `read` returned, which works until a body crosses a
packet boundary -- that is, until someone posts something. Every
connection now has a buffer of its own: bytes accumulate until
`Content-Length` says the request is all there, the handler runs, and
what arrived after it stays for the next turn, which is what makes
pipelining work. Limits come with it -- a head of 16 KiB and a request of
one megabyte by default, both answered with `413` rather than by growing
-- and so do the two things the loop has to know before there is a
`Request` to ask: the length, and whether the client said `Connection:
close`. `HEAD` is a `GET` whose body is not written, and the length still
says what it would have been.

**D123. A method is not reachable by its bare name.** A method lives in
the module's function table under its owner's name, and it was *also*
findable by its bare name -- so a plain call to a function called the
same thing went to the method, with no receiver. The verifier caught it
and nothing else did: `Bytes(value)` in a file with a class that has a
`Bytes` method called the method.

**D124. An offset too large for either addressing form is computed.**
A frame with a four-kilobyte buffer in it puts the locals after it out of
reach of both the scaled and the unscaled form, and the assembler
insisted one of them fit. It now folds the offset into x17 -- the
register the emitter keeps out of the allocator's pool for exactly this
-- through the immediate add and subtract, which are the only forms that
may name the stack pointer.

**D125. A layout is a question about a type, not about a file.** The file
being run may never have named the `List<int>` another file of the
package built, and a member table lives in a file's resolution: the type
came back with no fields at all, a field address fell back to offset
zero, and one field was written over another. A layout store now sees
every checked file's tables, and a field it cannot find is a trap rather
than a guess.

**D126. Mounting a router is prefixing its routes.** A route is a pattern
and a function, so `app.Mount("/api/v1", &api)` copies them with the
prefix in front -- and the match stays one walk over segments instead of
one walk per level. `OPTIONS` and `405` both answer with the methods a
path has routes for.

**D127. The IR has one string type.** Owned or borrowed, a string is a
pointer and a length; who frees it is tracked beside the value, in the
ownership flags, and not in the type. Keeping two made a branch that
yielded an interpolated string into a variable the checker typed as owned
a type mismatch -- which the verifier reported and which stopped the
whole module from compiling. Erasing `string` to `str` also widened what
the dev tier accepts: a function returning an owned string is now
compiled rather than left to the interpreter.

**D128. Wolang embeds behind a C ABI.** `include/wolang/embed/wolang.h`
is one header with no C++ in it: make a vm, load a package or some
source, call a function by name, and bind your own functions so the
script can call back. The compiled tier runs what it can and the
interpreter runs the rest -- the same arrangement `wolang run` has, which
is why a host gets machine-code speed without asking for it.

Three decisions worth stating. A host's functions are resolved through a
registry the runtime consults *before* the loader, so a program that
binds `host_log` gets the host's and not one that happens to be in the
process. A string crosses as a pointer and a length, which is what both
tiers hold one as, and a text the program answered is copied out --
because the program's own copy may live in an arena that is about to be
given back. And what a host may call is what the package *offered*: a
helper a script did not export is no more part of what it offers a host
than of what it offers another module.

This is not a sandbox, and the header says so: a script can declare
`extern "C"` and reach anything the process can, the same way the
standard library does.

**D129. A package says what it links against.** `[link] libraries = "SDL2,
m"` in a manifest, with `paths` and `frameworks` beside it. `wolang
build` passes them to the linker; `wolang run` has no link step, so it
opens them in the process instead -- which is what makes the same
`extern "C"` declaration work in all three tiers. A package binding a C
library is the package that knows which library, so this travels with it
rather than with the program.

**D130. `wolang bind` reads a C header.** The system's own preprocessor
expands it first, because a header is C and C has a preprocessor; what
this reads is *declarations*. Out comes a file of `extern "C" fun`,
constants from enum members, and aliases for simple typedefs -- a
starting point a person edits, not a binding pretending to be complete.
The honest parts are the limits: a struct by value, a variadic and a type
with no mapping are **reported** rather than dropped, a pointer to an
opaque struct is an address, and a callback parameter is an address too.
175 declarations out of the system's `math.h` compile as they are.

**D131. Three machines, because there are three answers.** The compiler
has an interpreter, an ARM64 tier and an x86-64 tier, and they have to
agree. A laptop can check two of them, which is the whole reason there is
a CI file: Linux x86-64 and macOS ARM64, debug and release, with the
sanitizers on one of them. Every job runs the unit tests, each library's
own tests, the differential fuzzer with the run number as its seed -- so
the programs differ run to run -- the examples, and the formatter against
itself. A second job links the embedding library into a C program and
runs a script through it, because an embedding API that is not called
from C is a guess.

**D132. A package starts with one command.** `wolang new game` writes a
manifest, a source file with a `main` and a test next to it, a README and
a `.gitignore` -- four files a person reads rather than twenty they skip.
`--lib` writes a library instead. Finding the standard library is the
only hard part: `WOLANG_STD` if it is set, a tree the new package sits
inside, `<prefix>/lib/std` next to an installed compiler, and the tree
this compiler was built from -- in that order.

`wolang test .` now runs the tests of what was *asked for* rather than of
everything it depends on, which is what makes a new package's output one
line instead of seventy-four. `--all` asks for the rest. And `cmake
--install` lays out exactly what `new` looks for.

**D133. An interpolated string is given back.** A program that built
strings in a loop grew without bound: `"value {n} of many"` allocates,
and nothing freed it. Three things were missing, and all three are the
same mistake in different places.

The *binding* owns it. An owned string is a type of its own (`Text`), so
the scope exit frees one the same way it destroys a class -- through the
flag that says whether the name still holds it. Which needed the
**unerased** type, because erasing a string to its two words is exactly
what loses the difference between owning the bytes and viewing someone
else's.

The *pieces* go back as they are consumed. A concatenation copies both
sides into a new allocation, so `"a {b} c {d}"` frees each intermediate
immediately: one live string instead of one per piece.

And a borrow is not a move. The checker records which uses are implicit
borrows -- an owned string handed where a `text` is wanted -- and the
merge from per-declaration results into the file's result was dropping
that list. So every string passed to a function looked like a move,
nobody owned it, and nobody freed it. Twenty million interpolations now
run in a flat twenty-six megabytes; before, the same program reached a
gigabyte.

**D134. The date header is written once a second.** HTTP dates have
one-second resolution, so a server answering tens of thousands of
requests a second can write it once and hand out the same string. Worth
the two globals: it is a calendar calculation and seven allocations per
response otherwise. The server does 39 700 requests a second in the JIT
and 42 900 as an ahead-of-time binary, in 2 MB of resident memory, and
28 500 with a log line per request -- which is what a line to standard
error costs.

**D135. A port, one listener per thread.** `ServeMany(app, port, most,
threads)` runs the same application from several threads, each with its
own listener on the same port through `SO_REUSEPORT`: the kernel spreads
connections across the queues, so no two threads wake for the same one.
Sharing a single listener instead makes every thread wake for every
connection, which is the thundering herd. Zero threads means one per
core, and a system without `SO_REUSEPORT` falls back to one listener --
which still works with one thread.

What a program keeps in a module-level `var` is now shared across those
threads, and whether that is safe is the program's business: a counter
wants `AtomicAdd` and a table wants a `Mutex`. Said out loud rather than
left to be discovered.

**D136. What `ab` found.** Three bugs, in a morning, from pointing a real
benchmarking client at the server.

A **1.0 client left hanging**: we answered `Connection: keep-alive` to
everyone, and an HTTP/1.0 client that did not ask for it waits for the
connection to close to know the response ended. The version is now part
of the request, and it decides what happens when the client says nothing.

A **global holding arena memory**: the cached date string was built
inside a request's `arena` block, so the block reclaimed it and the
global pointed at memory the next request wrote over -- which `ab` saw as
responses of differing length. The date lives in a global *buffer* now,
and the lesson generalizes: a global that holds a string built inside an
`arena` block is a dangling pointer the moment the block ends.

A **global with no initializer was no global at all**: `var date_bytes:
[40]u8` never got storage, because the declaration path checked for an
initializer before checking for mutability -- so reading it called a
function with no body. A `var` with no initializer is storage that starts
as its type's zero, which is what the runtime's storage already is.

With all three fixed: 53 900 requests a second on one thread and 58 900
on eight, with keep-alive and no failures -- and `ab` itself is the
bottleneck at that point.

**D137. The embedding API is something you ship.** A host wants more than
"load and call": a function looked up once and called every frame, the
script's module-level `var`s as settings it owns, the program's output in
its own console, a step limit for code it did not write, and a reload
when a file changes. All of that is in the C header now, and `wolang.hpp`
is the same API with the ceremony removed -- header only, over the C ABI
rather than over the compiler's own headers, so a host built with another
compiler still links.

Two things had to change underneath. Module-level `var`s were reset
before *every* run, which is right for a test and a differential check
(D88) and wrong for a host: it sets a global and then calls a function,
and the reset between the two wiped what it set. Runs now say whether to
keep them. And a host's output is captured by registering `write` in the
runtime's symbol table, which is consulted before the loader -- so a
program that prints reaches the host without knowing it.

Shipping it is the other half: `cmake --install` lays out headers, the
static and shared libraries, the standard library's sources, a CMake
package (`find_package(Wolang CONFIG)` → `Wolang::embed`) and a
relocatable pkg-config file. A release builds that layout per platform
and an embedding-only archive beside it. CI links a host project through
`find_package`, because an embedding API nobody links from outside the
tree is a guess.

**D138. A closure takes a copy of what it reads.** `|n| n * factor` now
works: the names a closure's body reads from the function around it are
copied into an environment, and the closure's value is the pair every
function value now is -- where the code is, and what it closed over. A
plain function closed over nothing, so a call through a value asks which
of the two shapes it has: one compare and a branch, in the IR, so every
tier gets the answer from the one place that knows it.

The environment lives as long as the call that made it, and is given back
when that scope ends. Which is the whole of the restriction: a closure
that captures may be passed *down* -- a comparator, a callback, a loop
body, which is what closures are mostly for -- but returning one, storing
one in a module-level `var` or putting one in a field is an error
(`E0136`), because what it captured would be gone. State that outlives a
call is what a class with a method is for; an environment with a lifetime
of its own is an allocation someone has to own, and a class is where this
language says who.

Three things had to give way underneath. A function value became two
words, which changed its layout, how it is read out of memory, and what
crosses into C -- a C function takes the code word, because it has no
idea what an environment is, and the same goes for a signal's handler
list. The checker records *which* names each closure captured, in the
order it read them, which is the environment's layout. And the merge from
per-declaration results into a file's was dropping that list, the same
way it dropped the borrows (D133) -- two lists added to a merge that
should have had them from the start.

`std.list` gained what closures are for: `Any`, `All`, `Count`, `Find`,
`Each`, `Fold`, `MapInPlace` and `Keep`, which is filtering without
allocating. And `List.Items()` hands out the part of its storage that is
in use, through a `std.mem.Prefix` that is the same pair-making
instruction `ViewFromRaw` is.

**D139. `wolang run --profile` says where the time went.** A sampling
profiler: a timer fires a thousand times a second, the handler writes
down the program counter, and afterwards each sample is attributed to the
function whose compiled code it landed in. Sampling rather than counting,
because counting changes what it measures -- and because a profile of the
machine-code tier is the one worth having. Samples that land outside the
program's own code -- the runtime, the allocator, the kernel -- are one
line rather than hidden, because "most of the time is in the allocator"
is an answer.

It names a generic instantiation and a closure the way the rest of the
compiler does, so `@Partition<s32>` and `@closure$2` are what a profile
of `Sort(numbers, |a, b| a < b)` shows.

**D140. `wolang repl` is a compiler in a loop.** Each line becomes a
*file* of one session package: a declaration joins the session, and
anything else becomes a function that shows what the line came to. One vm
for the whole session, so a `var` keeps what was written into it and a
`fun` stays declared; a line that does not compile is dropped by
rebuilding the session from the lines that did. `<?>` at the end of what
a line printed means its value was nothing -- the line was a statement --
so what it printed itself is what gets shown.

It is built on the embedding API rather than on anything new, which is
how three real bugs in that API came out:

A host's source could not **say `need`**. A file added by a host belonged
to no package, so `need std.io` resolved to nothing. Host sources now
belong to one package of their own -- made once, not per call -- whose
dependencies are every package the host loaded.

A **module-level `var` of another file** was read as a constant: lowering
only knew its own file's globals, so a cross-file read gave back the
value the variable started with and threw every write away. It is the
same instruction with the same token now, built from the declaration the
binding points at.

And **a name is not unique across a program**. `wolang_vm_call` found a
function by name and the compiled tier then looked *its* name up again --
so a session that declared `Peek` reached `std.json`'s private `Peek`
instead, with the wrong number of arguments. Lookups prefer the most
recently loaded definition with a matching arity, and the call is made by
the declaration a function came from rather than by its name.

The interpreter's own `Print` now reaches a host too: it collects what a
program printed rather than writing it, and a host that asked for the
output gets that as well.

**D141. A dependency may be fetched, and what was fetched is written
down.** `engine = { git = "...", tag = "v1.2.0", dir = "lib/engine" }` in
a manifest, `wolang fetch` to get it, and `wolang.lock` recording the
repository, the revision that was asked for and the **commit it resolved
to** -- which is what makes a second machine build the same code.

Three decisions. Fetching is `git` through the `git` on the path: a
package manager that cannot be told "use the git you already have" is one
that fights the machine it runs on. Only `fetch` reaches the network --
`check` on a missing dependency is an error with a fix in it (`E0061`),
because a compiler that downloads something because a file was saved is
not one anyone can trust. And a revision is required: a dependency
without one changes under you, and the lock file would have nothing to
record.

Where a checkout lands depends only on the repository and the revision,
so two packages wanting the same dependency share one copy, and two
revisions of the same repository are two directories.

**D142. `--record` and `--replay` make a run happen again.** Everything
the machine answers -- the clock, standard input, the process id -- is
written down on the way out and handed back on the way in, in order. So
"it only goes wrong on the server" becomes a file: record there, replay
on a laptop, and the program sees the same clock, the same input and the
same random numbers, because the seed came from the clock.

The journal is text, with the bytes in hexadecimal: one that cannot be
read in an editor is one nobody looks at when the replay disagrees. The
shims take over through the runtime's symbol registry (D137), so a
program reaches them without knowing they are there -- and a run with no
journal touches none of it.

What it does not do is make threads deterministic: they interleave as the
machine decides, and a program whose outcome depends on that is not fixed
by this. Said out loud rather than discovered.

**D144. A client, because serving is half of HTTP.** `Fetch(url)`,
`PostJson(url, body)`, and `Open`/`Ask` for a program making several
calls on one connection -- over `std.net`, which is the same socket floor
the server stands on. A URL is taken apart into a host, a port and a
target; the answer is read until `Content-Length` says it is whole, which
is a loop because a read stops wherever it likes.

Two things are said out loud rather than pretended. There is no TLS:
`https` answers `NeedsTls`, because a TLS implementation is not something
this language should contain and a package binding one through `[link]`
is where it belongs. And a reply's bytes are one allocation that the
reply's parts are views into -- so they outlive the call, and a program
making many calls puts them in an `arena`.

Writing it found a checker bug that had been hiding behind error
propagation: `Ok(value)` in a `match` on a `T!` never got a type, because
`Ok` is not a declared member of anything. An error type is accepted
everywhere, so nothing complained until a *reference* to such a binding
turned up -- `&<error>` against `&Thing`, a long way from the cause.

**D145. Output is buffered when a program says so.** `std.io.Out`
collects and writes in blocks: fifty thousand lines through `Print` is
fifty thousand system calls and half the time in the kernel; through
`Out` it is one write per sixty-four kilobytes, and the same program
spends no system time at all.

Explicit rather than automatic, because the two callers want opposite
things: a program whose output is a log wants every line out *now*, and
one writing a report wants it fast. `Flush` is where a program says
which, and a writer that goes out of scope flushes itself -- one that
dropped the last line would be a writer nobody could trust.

**D146. A deque and a heap, because a list is not either of them.**
`std.deque` is a ring over one allocation: pushing and popping at either
end is arithmetic, which is what a work list and a breadth-first walk
need and what taking from the front of a `List` cannot be. Growing it
lays it out straight again, so the arithmetic stays the same afterwards.

`std.heap` is a binary heap whose order comes in as a function value, the
way `Sort`'s does: `HeapNew<int>(|a, b| a < b)` is a smallest-first
queue. Sorting a whole list to take one element off it is the thing it
exists to avoid.

Both `Pop` the same way: the caller checks `Len` first. An optional of
`T` would be a second shape to lay out for every `T`, and a queue with
nothing in it has nothing to give.

**D147. A parallel loop is a closure and a range.** `ParallelFor(count,
threads, |from, to| ...)` splits `[0, count)` into chunks and runs one
per thread, each with a range to itself -- so the body is an ordinary
loop and the work is divided with no locking. Zero threads means one per
core, the calling thread takes the last chunk (so a one-thread loop
spawns nothing), and the remainder is spread one element per chunk rather
than left to the last. Two hundred thousand elements of real work: 851 ms
on one thread, 142 ms on eight.

It is declared with no body, like `TokenOf` and `RunSystems`: what the
runtime needs is the *two halves* of the body's function value -- the
code and what it closed over (D138) -- and taking a function value apart
is something only lowering can do. The language has no way to do it, and
should not.

The reference tier runs it in one go, with the whole range: the
interpreter is what every test is compared against, and the sequential
answer is the one that has to match. Which also says what a body may not
do -- write to what it captured, because a capture is a copy and here
there is one per thread. A result comes back through storage the body was
given, by index.

**D148. Names, through the system's resolver.** `std.net.Connect` took a
dotted address, which means a client that could not reach anything a
person would type. `Resolve` asks the system -- `getaddrinfo`, so
`/etc/hosts`, the resolver's configuration and mDNS all answer the way
they do for every other program on the machine. A language with its own
DNS would answer differently from the machine it runs on.

An address is 64 bits wide here, because `128.0.0.0` already does not fit
a signed 32-bit one -- which is what made resolving a real host overflow.

**D149. The rest of a filesystem.** `Exists`, `KindOf`, `SizeOf`,
`ChangedAt`, `Remove`, `Rename`, `MakeDirectory` and `List`: a tool needs
all of them, and a program should not have to declare `stat` itself. A
listing comes back as names separated by line breaks rather than as a
list -- the caller splits it with `std.text.Pieces`, which is what it
would do with a list anyway, and this way the whole listing is one call
and one allocation.

**D150. TLS is bound, not implemented.** `lib/tls` is OpenSSL through
`extern "C"`, with `[link] libraries = "ssl, crypto"` in its manifest: a
language that shipped its own cryptography would be shipping its own
CVEs. The certificate is verified against the system's roots *and* the
name is checked against the certificate -- TLS that checks only the first
is TLS any server can answer.

Two things came out of writing it. `SSL_set_tlsext_host_name` is a
*macro* in OpenSSL, so a binding reaches what it expands to --
`SSL_ctrl` with control code 55 -- which is exactly why `wolang bind`
produces a starting point rather than a binding (D130). And a bare
library name goes through the loader's own search, which on macOS finds a
shim for `libcrypto` and warns that it is being loaded unsafely: a
manifest that says where the library is means it, so the named paths are
tried first.

**D151. The client does what a client has to do.** A built `Call` --
method, headers, body, query, basic or bearer authentication, timeout,
redirect limit, certificate verification -- and `Perform` runs it:
`https` over the system's TLS, redirects followed (a `303` becomes a
`GET` with no body, which is what the status means), `chunked` answers
put back together, and a timeout so that a server which never answers
does not become a program that never returns.

The plain and the TLS read loops are the same loop twice rather than one
loop over an ability: a socket and a stream are two types with the same
three methods, and an ability with a method per direction is more
machinery than the duplication costs.

**D152. Nothing goes into a block nothing reaches.** An `if` whose every
arm returns or breaks has a join block nothing jumps to -- and lowering
put the *enclosing* structure's jump in it, which the verifier rightly
called a block unreachable from the entry. The builder can now be
poisoned: after such a join, everything appended is dropped until the
next block begins. An empty unreachable block is something both the
verifier and the passes already ignore.

**D153. Another program, with pipes.** `std.process.Run` hands a string
to the shell and answers an exit code: enough for a build step, wrong for
a tool. `Start` and `StartWith` are `fork` and `exec` with three pipes --
the output, the error stream and the code, and the input written to it --
and no shell, because a command built out of a string is where the oldest
hole in computing lives. The command is split on blanks with quoted runs
kept whole, which is how a path with a space in it survives without a
shell being involved.

Two lowering bugs came out of writing the binding. What goes into a place
is of the *place's* type: `var p: *u8 = &xs[0]` makes a pointer that may
be written where the place holds one that may not, and a slot whose
stores disagree with its own type is one the passes promote into a block
parameter that disagrees too. Both the declaration and the assignment now
coerce, which is the same rule as D127 one level down.

**D154. Arguments past the registers.** Eight integer registers carry
arguments on arm64 and six on x86-64. Past those, the dev tier indexed
its table of argument registers without checking -- so a call with more
arguments read whatever followed the table in memory and passed nonsense,
in silence, while the tier's own refusal list claimed such a function was
skipped. Two bugs that hid each other: the refusal made the garbage rare,
and the garbage made the refusal look prudent.

Both now do what the ABI says. A frame reserves an outgoing area at the
stack pointer, sized by the hungriest call in the function, and arguments
past the registers are stored there; a parameter past the registers is
read from above the frame, where the caller left it. The refusal is gone,
because there is nothing left to refuse.

The interpreter's way out of the language had the same bound and reported
it as "symbol was not found", which sends a person looking in the wrong
place. It builds one thunk per number of stack arguments -- so the copies
are a fixed sequence of immediate-offset stores rather than arithmetic on
the stack pointer -- and says `kTooManyArguments` when there are more
than it will build for.

This is what `glTexImage2D` needs (nine arguments), and what every
windowing and graphics binding needs after it.

**D155. A window of our own.** `platform/` is this project's backend for
each system -- Cocoa, Win32, X11 -- behind one C API: a window, what a
person does to it, a buffer of pixels, a GL context and a queue of sound.
Not a binding to someone else's windowing library: that would be a
dependency to ship, a second layer to debug, and a thing to install
before a program that draws a rectangle can run.

It is its own shared library rather than part of the runtime, so a
program that draws nothing links nothing -- a compiler that pulled in
Cocoa to print a number would be paying for a window it never opens. The
`gfx` package reaches it through `[link]`, the same mechanism `tls` uses
for OpenSSL (D150), and a relative path in a manifest is now relative to
the package rather than to wherever the command was run.

Two ways to put something on the screen, one window. `Present` hands over
pixels -- `0xAARRGGBB`, rows top to bottom -- which is what a tool, a
plotter or a software renderer wants, and what the drawing in `gfx` fills
in. A GL context is the other: `StartGl`, draw with OpenGL, `Swap`. The
GPU is reached by calling it, not by an abstraction over it.

Sound is a queue rather than a callback. A callback would be the system's
audio thread calling into a tier that may still be compiling; a program
that makes sound has a frame loop to push from, and a ring buffer with
one writer and one reader needs no lock.

What a backend nobody has run is worth is nothing, which is why
`examples/window` exists: it opens a window, draws a moving wave at a
hundred and fifty frames a second, reads the keyboard, plays a tone and
says so in its title.

**D156. `ptrnull`.** The language could turn an address into a number and
not the other way around, so a binding had no way to write NULL -- half
of C's functions take one and the other half give one back. `lib/tls`
worked around it by freeing a one-byte buffer and keeping the address: a
pointer that is neither null nor valid, as a sentinel.

`ptrnull` is the address no object has. It takes whatever pointer type
the context wants, like `null` does for optionals, and `*mut u8` when
there is none -- the pointer every other converts from without breaking a
promise. A number as an address (`0x1000 as *mut u8`) is allowed too, in
`unsafe`, for a binding that was handed one.

**D157. The machines, checked.** Linux on arm64 joined the matrix: same
architecture as the laptop, different system, and the one combination
that had never been built. A language that says it runs somewhere and
never builds there is making a claim about a machine nobody has.

**D158. Sockets through the runtime.** `std.net` and `http` each bound
the system's own `socket`, `send` and `close`. That works on every system
that copied Berkeley's interface, and Windows is the one that did not
quite: a socket there is a handle rather than a descriptor, closing one
is `closesocket`, the library has to be started before the first call,
and `send` takes an `int` where everything else takes a `size_t`.

A binding cannot be written twice in a language with no conditional
compilation, so the difference lives in C now -- `wolang_socket_open`,
`connect`, `listen`, `accept`, `send`, `receive`, `close`, and the three
options a server sets. Both Wolang packages are the same everywhere, the
`sockaddr_in` nobody could name is built where the headers are, and the
server still answers fifty-three thousand requests a second, which is
what says the layer costs nothing.

**D159. Windows.** The rest of what a system is: `VirtualAlloc` for the
pages a tier compiles into, `WSAPoll` for readiness, `CreateProcess` and
three pipes for another program, `LoadLibrary` and `GetProcAddress` for a
binding, `_open` in binary mode so a file round trips, and the standard
library for threads, locks and the filesystem -- `pthread_create` and
`CreateThread` differ in every detail and in none that matters here, and
a second implementation of a lock is a second place for a race to hide.

Two things that look like details and are not. Writable *and* executable
memory at once is what every exploit of the last thirty years wanted, so
pages are written, then made executable, on every system. And a command
is split into words by the same code everywhere and joined back together
with this system's quoting for `CreateProcess` -- so that what a caller
wrote means the same thing on both.

CI builds and runs it, because a port nobody runs is a port that does not
work.

**D160. Code for another machine.** `--target <triple>` on `wolang
build`, and every target this LLVM has rather than this machine's only:
a compiler that can only emit code for the machine it runs on cannot
build the Windows binary from the laptop. The driver grew from
seventy-six megabytes to a hundred and sixty-seven, which is what every
target costs and what every other toolchain pays.

What comes out is an object file, and the message says so: linking it
needs that machine's runtime and that machine's linker, neither of which
this build has. A triple that *is* this machine links the usual way, so
`--target` is not a different command for the same thing. Verified by
compiling the same program for Windows, Linux on two architectures,
WebAssembly and RISC-V, and reading what the object files say they are.

**D161. `Print` in a file compiled on its own.** A single file links no
standard library, so `Print` there is a declaration with no body: the
interpreter answered it with an intrinsic, the compiled tiers could not
answer it at all, and a binary built from such a file trapped the moment
it printed. One runtime symbol, pointed at by the one declaration that
has no definition, and all three tiers do the same thing.

**D162. One document, four formats.** JSON, TOML, INI and YAML are four
spellings of the same six things: a nothing, a truth, a number, some
text, a list and a table. `std.doc` is that value, and every format
reads into it -- so a program that reads configuration does not care
which file it came from, and `Get(value, "server.ports.0")` is learned
once. Written as four parsers over one tree rather than four trees.

**D163. A nested generic is not two arguments.** `List<List<T>>` ends on
one token that closes two lists, and the comma after it belongs to
whatever the type is *in*. The argument loop ate that comma as a
separator, so `fun f(a: List<List<T>>, b: int)` parsed as a list with two
arguments and a function with one parameter. The close is checked before
the comma now.

**D164. An owned string freed where it was made.** A string built by
interpolation is owned and freed at the end of the scope that made it;
one stored in a structure that outlives that scope is a dangling
pointer. `std.csv` built column names that way and read them back as
rubbish. A builder's `Release` is what hands a string to something that
will outlive the call -- which is the wart D127 left, written down here
because it bit.

**D165. Tags, where a document has no room for them.** YAML's standard
tags decide how a scalar is read -- `!!str 3` is text and `!!int "7"` is
seven -- and an unknown tag leaves the value alone. A document value here
has no room to record which tag it carried, and a reader that refused a
file over a tag it did not know would be a reader nobody could use.

**D166. A matcher that answers.** `std.regex` is a backtracking matcher
over a compiled program: literals, classes, anchors, groups,
alternation, the repetitions and their reluctant forms, and two flags.
No look-around and no back-references, because each turns a matcher into
a different machine.

Backtracking has exponential cases -- `(a+)+b` on a line of `a`s is the
famous one -- so the walk has a budget. Past it the answer is no rather
than never, which is the honest thing for a library to do to a program
that did not choose its own pattern.

**D167. Indexing through a reference.** A field and a method already
reach through one; an index did not, so a `&mut [40]int` parameter was
an array the callee could not read. Through a reference now, in the
checker and in lowering -- the place is rooted at the address the
reference holds rather than at where the reference itself lives.

**D168. Arrays in memory, in the reference tier.** Two bugs, both of them
the same shape: an array has elements at a stride and a struct has fields
at offsets, and the interpreter used the second rule for both. A `[4]int`
field written to the heap was skipped entirely -- zero in the reference
tier, nine in machine code -- and `rows[1][2]` on a `[2][3]int` added the
indices instead of stepping into the row, which walked off the end.

Found by writing a regular expression engine, which keeps a fixed array
of group positions in a struct and copies it through a list. Which is the
rule this project keeps relearning: a real program finds what a test for
a feature does not.

**D169. A type in brackets is still a type.** `ArrayNew<[4]int>(1)` was
read as a comparison with an array literal, because the heuristic that
tells a generic call from a `<` did not allow brackets between the angle
brackets. Nor a function type, so `Keep<fun(int) -> bool>(f)` was the
same. Both are allowed now.

**D170. Randomness that is not a sequence, and hashes that are ours.**
`std.random` is a generator: fast, repeatable, and exactly wrong for a
token. `std.crypto` takes its bytes from the system -- `getentropy`, or
the cryptographic provider on Windows -- because a language that rolled
its own would be rolling its own mistakes.

SHA-256 and SHA-512 are written in Wolang rather than bound: two hundred
lines of arithmetic with published vectors, no keys to mishandle, and a
language that cannot hash a salt without a C library has a hole in it.
Anything with a cipher or a key exchange in it is still `lib/tls`, which
binds the system's own (D150).

The arithmetic wraps where SHA says it wraps -- written as a wider add
and a mask, because every operator in this language traps on overflow and
here the wrapping *is* the algorithm.

**D171. A byte is one byte wide.** The interpreter wrote an integer
through a pointer using the width of the value it happened to be
carrying, not the width of the type being written. A `u8` went out as
eight bytes and took the seven after it with it -- which is how a digest
in a heap buffer came back with its first half belonging to whatever was
written next to it. Found by the sanitizer, in one line, after an hour of
reasoning about allocators.

**D172. Strings have an order.** `a < b` on two strings compared the
*pairs* -- a pointer and a length -- so the answer was about where the
strings happened to live. Now it is a call into the runtime that compares
bytes, which for UTF-8 is also the order of the code points. The three
tiers agreed on the nonsense before and agree on the answer now.

**D173. Both sides of an operator are one type.** `48 + byte` built an
add whose operands were a literal's type and a byte's. Nothing complained
until two such returns met at an inlined function's continuation, where
one was wider than the other.

Three fixes, smallest first: the side that is not a bare literal decides
the type, both operands are cast to it, and the inliner casts a returned
value that does not match the call's type. The second is the one that
mattered -- an operator whose operands disagree has no meaning in this
IR, and the verifier said so as soon as anything looked.

**D174. The logo is a script.** `assets/make_art.py` draws the mark, the
wordmark and the mascot, and writes both the SVG a page embeds and the
PNGs an extension ships. One source for both, so they cannot drift; a
scanline rasteriser and a PNG writer in a hundred lines, so the build
needs no drawing library. The mark is a wolf's head whose ears are the
outer peaks of a W, which is the two things the name is.

**D175. A `.vsix` without npm.** A Visual Studio Code package is a zip
with two manifests in it, so `tools/package/make_vsix.py` is a zip with
two manifests in it. What it packages is an extension written in plain
JavaScript with no dependencies -- a framed JSON-RPC client and the
translation between what LSP says and what the editor's API wants, which
is two hundred lines.

What that buys: the extension builds and installs on a machine with no
npm, no network and no login. What it costs: `vscode-languageclient`
would have done the two hundred lines. The trade is worth it for a
language whose whole point is that the compiler answers every question --
an extension that cannot be built is one nobody can try.

**D176. Zones come from the system.** A time zone is not arithmetic: it
is a database of what each government decided, revised several times a
year, and every machine already has a copy that everything else on it
agrees with. `ZoneOffset`, `ZoneName`, `CivilIn` and `EpochIn` ask that
copy. `EpochIn` asks twice -- the offset depends on the moment and the
moment is what is being worked out -- which is what gets the hour after a
clock change right.

`ParseIso` reads the one format worth building in, because it is the one
every machine writes.

**D177. Deflate, written here.** `std.compress` is a real compressor --
a hash chain of three-byte matches, fixed Huffman codes -- and a
decompressor that reads stored, fixed and dynamic blocks. Bound to zlib
it would have been shorter; written here it is arithmetic over bytes
with a published format, which is the same argument as the hashes
(D170).

The test that matters is not the round trip: it is that the system's
`gzip -t` says the output is valid and `gzip -dc` prints the input back,
and that what the system's gzip wrote reads here.

`std.mem.Suffix` came out of it: `Prefix` had no other half, so a reader
walking a buffer had to copy what was left. Both are intrinsics, because
only the tiers know how far along an element is.

**D178. `[]u8!` is a slice that may fail.** The element of a slice is
parsed without suffixes now, so `!` and `?` belong to the whole type --
which is what a function signature means by it every time. A slice of
optionals is `[](T?)`, and a type in brackets is that type: `(T)` is `T`
and `(T,)` is a tuple of one.

**D179. The rest of a filesystem, and a child's world.** Where the
program is, where the person's things are, where something temporary
belongs, what a path really is with `.` and `..` worked out, copying,
appending, walking a tree, and taking one away. And for a child process:
somewhere to run and something to add to its environment -- added rather
than replacing, because a child with no `PATH` cannot find anything.

**D180. A file kept open.** `std.fs` reads and writes whole files, which
is the right shape for a configuration file and the wrong one for a
store: a store appends records and reads them back at the offset it
remembers. So the runtime has the handful of calls that needs -- open,
read at, append, size, sync, truncate, close -- and `fsync` is in there
because it is the whole difference between a store and a cache.

**D181. A store is a log and an index.** `lib/db` appends every write --
key, value, a checksum -- and keeps an index in memory saying where the
newest record for each key is. Opening reads the log once; a record whose
checksum does not match, or that the file ends in the middle of, is where
the log stops, and the file is truncated there. Which is exactly what a
machine that lost power during an append leaves behind.

Not a B-tree: a tree is the right answer when the index does not fit in
memory, and it is a month of work to get page splitting, the free list
and crash recovery right. A log is a day's work and survives a crash by
construction. The index is *sorted*, so ordered walks and ranges work --
the one thing a log-structured store usually gives up.

Verified the way the claim deserves: fifty keys written, a torn record
appended by hand, and the store reopened with all fifty intact and the
file trimmed back.

**D182. A value returned from a fallible.** `return 7` from a `-> int!`
returned a bare seven where a pair was expected: the checker records the
literal as `int!` -- it adapted it -- and lowering built a `const.int`
*typed* `int!` and then saw nothing left to wrap. The interpreter read
the seven as a struct and found no field; the compiled tiers read the
`match` as unreachable.

A literal is built at the element type now, and the wrap is the `Coerce`
that was always there. The lesson is the one this project keeps
relearning: what a value *is* comes from the instruction that produced
it, and what the context wanted is a separate question (D111, D173).

**D183. The manual's programs compile.** A manual full of programs that
do not build teaches the wrong thing and nobody finds out.
`tools/package/check_docs.py` pulls every fenced `wolang` block that
claims to be a program -- one with a `fun main` in it -- writes it into a
package and runs `wolang check` over it. CI does this on every commit.

It found nine broken examples the first time it ran, in a file written
an hour earlier, by someone who had the library open in another window.

**D184. This project is built with clang.** Not a preference: the
compiler uses clang's overflow builtins for the checked arithmetic every
Wolang operator compiles to, `__int128` for the wide multiply a constant
divisor becomes, and statement expressions in the trap machinery. The
configure step says so and stops, rather than letting a build fail two
hundred errors later.

On Windows that means clang-cl -- clang's compiler, Microsoft's flags and
headers -- which is what the LLVM installer provides and what the
`windows` preset selects.

Trying it with Microsoft's own compiler turned up eight portability bugs
that were real under clang-cl too, and they are fixed: `setenv`,
`clock_gettime`, `popen`, `_longjmp` and `sys/time.h` behind one name
each; `std::thread` and `<mmreg.h>` missing from the Windows backend; a
cursor handle that needed a wide pointer; and `file_clock::to_sys`, which
one standard library has and the other does not -- so the difference
between the two epochs is measured once at run time rather than named.

**D185. Installing is one line.** `install.sh` and `install.ps1` work out
which build the machine wants, download it, check it against the
published `SHA256SUMS`, unpack it under `~/.wolang` or
`%LOCALAPPDATA%\Wolang`, and put that on the PATH. Nothing is written
outside the prefix and nothing asks for a password -- a language that
needs `sudo` to try is a language fewer people try.

POSIX shell rather than bash, because this runs on a machine where
nothing is installed yet. `--from-source` is there for a platform with no
build published, and it insists on clang like everything else (D184).

The release now carries every platform -- Linux on two architectures,
macOS on two, Windows -- plus the checksums the installers read and the
editor extension.

**D186. The whole compiler as one file.** `make_amalgamation.py` inlines
every header and every source into one `.cc` that builds with a plain
`clang++ -std=c++20 -c`, beside the `wolang.h` a host includes. For a
host that wants to embed the language and not a build system, that is the
whole delivery: two files.

Three things had to be true for it to work. An `.inc` is an X-macro table
and is included several times on purpose, so it is inlined every time
rather than once -- the first version produced an empty opcode table and
a static assertion that fired a long way from the cause. A header's
include guard has to come off, or every include inside it looks
conditional. And half a dozen file-local names were too generic to share
one translation unit: four `Entry` structs, two `State`s, two `Checker`s,
five `kNames`. They now say which file they are in, which is better
anyway.

The emitters needed more than a rename: both kept their tables in an
anonymous namespace beside each other, so they now sit inside
`arm64`/`x64` instead. Two anonymous namespaces in different enclosing
namespaces are different namespaces.

CI builds the amalgamation and runs a host against it on every commit.

**D187. One name, one file.** On Windows a shared library brings an
import library with it, and ours was called `wolang_embed.lib` -- the
same name the static library has, which is two build rules writing one
file and a build that stops. The import library has its own name now.

**D188. A real installer on Windows.** A `.msi`, built by WiX from
`tools/package/wolang.wxs`: it installs under Program Files, adds its
`bin` to the system PATH as a component -- so removing the product
removes the entry -- registers itself in Add/Remove Programs with an
icon, and upgrades in place rather than sitting beside the old version.
The payload is whatever `cmake --install` lays out, harvested at build
time, so a file added to the project does not need a line in an
installer as well.

The icon is the same mark as everything else, written as an `.ico` by
the script that draws the rest (D174): a header and a PNG per size,
which is what the format has allowed since Vista.

The PowerShell script stays for people who want a copy in their home
directory and no administrator.

**D189. Every file says what it needs.** `include/wolang/base/unicode.h`
used `std::string` and included `<string>` nowhere: libc++ drags it in
through something else, Microsoft's library does not, and the build fell
over on Windows with an error about a type that has been there all along.
There were eighty-one of those.

`tools/package/check_includes.py` asks the question on every commit --
which standard names a file uses, and whether it includes the header each
one comes from. It is a heuristic over the names this project actually
uses, not a model of C++; what it buys is that the next one is found by
CI rather than by somebody on another operating system.

**D190. A type comes with the value.** `FindAll` hands back a
`List<Match>`, and the caller never mentions `List`. Calling `.Len()` on
it produced an *error type* -- silently, with no diagnostic -- and the
program trapped at run time with nothing explaining why.

The cause: member tables were merged into a file's resolution from the
modules it imports directly, and `List` belongs to `std.list`, which the
caller never imported. So the caller had a type it knew the name of and
no members.

The merge walks the import graph transitively now. That also means a file
is invalidated when a transitively imported file changes -- which is
correct, because its types' members did change. The cost of the old
behaviour was a class of silent miscompilation in every program that
passes a container across a package boundary, which is every program.

Found by writing the cookbook: the example that searched a file with a
regular expression did not work, and the manual is where a library's
shape gets used the way a stranger would use it (D183).

**D191. The first fix.** `Fix` and `FixEdit` had been in the diagnostic
since the beginning and nothing produced one. `W0101` -- a name nothing
uses -- now carries the change that silences it: an insertion of `_` at
the name, which is exactly what the message already told a person to do.

Two ways to apply it. The language server answers
`textDocument/codeAction` with the fixes of whatever diagnostic the
cursor is on, translated and nothing else -- the compiler decided the
change, the server does not have a second opinion. And `wolang fix`
applies them without an editor, over a whole tree, printing what it would
do unless `--write` is given.

Edits are applied from the end of the file backwards, so that an earlier
offset is still where it was.

**D192. A constant divisor is a shift.** The dev tier emitted a hardware
division whatever the divisor was. For a constant power of two -- `/ 8`,
`% 16`, `/ -4` -- it is now a shift, with the bias that makes a signed
shift round towards zero the way division does, and the zero check goes
away because a constant divisor is not zero.

Worth doing in this tier rather than leaving it to LLVM, because this is
the tier `run` and `test` use, and `% 10`-shaped code is in every number
printer.

The general case -- a multiply by a magic constant for a divisor that is
not a power of two -- is still a division here. It needs a "high half of
the product" opcode in the IR, and the value of adding one is smaller
than it looks: a non-power-of-two constant divisor in a hot loop is rare,
and `wolang build` goes through LLVM, which does it.

Both back ends have the same arithmetic in their own instructions, and
the reference tier still divides -- so every case is compared against a
division by the differential fuzzer and by the test that walks eighty
values through six divisors.

**D193. A class is counted, not leaked.** `class` allocated and never
gave anything back unless the class had a `deinit`. Now every object
carries a count of how many places hold it, and the last one to let go
runs the destructor and frees it.

The count lives in the word *in front of* the object rather than inside
it: no layout changes, so a field is where it always was and the three
tiers go on agreeing about offsets without being told anything. The
compiler only ever sees the address it was given.

Where the counting happens: `new` answers with one, a store into a place
that outlives the expression takes a reference, a value on its way out of
a function keeps one, and a binding releases what it holds when its scope
ends. A value that arrived already counted -- from `new` or from a call --
is handed on rather than counted again, which is what stopped the first
attempt from ever reaching zero.

What is *not* counted: an argument. A function borrows what it is handed,
because the caller is holding it for the duration of the call, and
counting there would be two atomic-shaped operations per call for nothing.
A callee that keeps what it was given does so with a store, and the store
is what takes the reference.

Two things still leak, and both are written down rather than hidden: a
cycle, which is what reference counting cannot see; and a handle
overwritten in a field rather than dropped, because releasing the old
value would mean reading the place back first. Leaking there is the right
way round -- the other mistake frees something that is still in use.

`Owns` and `Releases` are deliberately different questions. `Owns` is
"does handing this on give it away", which is what makes a `World` passed
to the functions that work on it a borrow rather than a move (D87).
`Releases` is "does this binding hold a reference". Conflating them made
every class parameter a move and quietly freed a world halfway through
the function that was using it.

**D194. The trampoline takes stack arguments too.** The body has read
arguments past the registers off the stack since D154. The trampoline a
host call enters had not been told: it wrote past the end of its register
table, which the sanitizer called a global buffer overflow and a release
build would have called a wrong answer. The x86-64 side had an honest
refusal in front of it instead -- "takes more arguments than registers",
which left every such function to the interpreter.

**D195. A byte operation needs REX.** Without the prefix, registers four
to seven in an x86-64 byte operation name `ah`, `ch`, `dh` and `bh` --
the *high* byte of another register. The assembler forced it for a store
and for `setcc` and not for the dozen other instructions that have a byte
form. Forced in the one helper now, because a dozen call sites is a dozen
chances to forget.

**D196. Scratch zero is the accumulator.** Three places in the x86-64
emitter asked for scratch zero and then wrote the accumulator, which is
the same register: the address of an array element became twice the base
of the array, and wrapping a value in an optional stored the tag to the
address the tag had just overwritten. A program that put either in a
place the allocator had to spill crashed, and the crash was in generated
code with no source line on it.

Found by running the x86-64 build under Rosetta on an Apple Silicon
machine and then dumping what it emitted: `wolang asm --raw` prints the
bytes, and a disassembler says the rest. That is now the way this tier is
checked without a second machine -- the whole suite ran, and 94 tests in
it crashed.

**D197. What the x86-64 tier had quietly stopped doing.** Two more from
the same session: a virtual call read the token out of the object a
second time, as if the token were the handle, and called the lookup with
whatever the first field happened to hold -- lowering has read it already,
and the other back end passes it straight through. And a call with more
arguments than registers refused the whole function, from before the
outgoing area worked.

**D198. A narrow answer is only as wide as its type.** A function that
answers a `bool` or an `s32` may leave anything it likes in the rest of
the register, and both back ends took the whole sixty-four bits. So
`wolang_world_alive` answered false and the program saw true, because the
high bits were someone else's arithmetic. The result is now narrowed to
its type where it is taken, which is one instruction and the difference
between right and nearly right.

**D199. A temporary may not be where the operand is.** The shift-instead-
of-divide sequence (D192) was written twice, and the x86-64 copy used the
low scratch registers for its own temporaries -- which is where the
operands of the instruction that called it live. So `% 64` read a value
that the sequence itself had just written over, and `std.utf8` encoded a
code point into bytes that did not fit a byte.

The lesson is about scratch registers rather than about division: a
helper that is handed an operand register must copy it out before it
writes any scratch it has not been told is free.

**D200. The swap slots on x86-64 were a word wide.** Values that trade
places around a loop go through a slot each, and a slice needs a slot
wider than a word -- in eight bytes it keeps its address and loses its
length. The arm64 emitter was told this (D107); this one was not, and
wrote a sixteen-byte slice into an eight-byte slot, over the slot beside
it. A loop carrying a slice and a counter came back round with the
counter in the slice's length.

Five of the standard library's tests failed on that one bug -- base32,
SHA-256 over long input, signing, a socket talking to itself, and the
parallel `for`. All five are loops that carry a slice.

The rule this leaves behind: a bug fixed in one emitter is a bug to look
for in the other, and the comment that explains the fix should be read as
a question about the other machine.

**D201. Windows x86-64 has its own calling convention.** The x86-64
tier spoke System V everywhere, so on Windows every compiled function
read its arguments out of `rdi` and `rsi` while the host had put them in
`rcx` and `rdx`: `Add(2, 3)` answered 0, and every test that reached
machine code failed or crashed. The port was never run -- CI lives in a
GitHub workflow and the repository is on GitLab.

One convention per machine, not one inside and another at the edges: the
runtime calls compiled code directly (a parallel `for`, a system of the
ECS), so every body has to be callable from C. On Windows that means four
argument positions shared by integers and floats, thirty-two bytes of
shadow space under every call, `rsi` and `rdi` saved by the callee (the
tier uses them as scratch, so every body saves them), floating-point
scratch only in `xmm0`-`xmm5` (the rest belong to the caller), and a
probe of every page when a frame is bigger than one. Where an argument
goes is answered by one cursor shared by the prologue, the call, the
entry stub and the frame layout, so they cannot disagree again. The
interpreter's thunk into C got the same treatment.

**D202. A trap on Windows leaves without unwinding.** `longjmp` there is
an unwind: it walks every frame between the trap and the landing pad
through the exception tables, and generated code has none, so a trap was
`STATUS_BAD_FUNCTION_TABLE` instead of a trap. Nothing between the two
holds a destructor (D59), so the jump buffer's frame is cleared after
`setjmp` and the jump becomes a plain register restore -- the same thing
`_longjmp` is elsewhere.

**D203. The standard library asks the runtime, not POSIX.** `std.io`,
`std.fs`, `std.process` and `std.time` declared `write`, `read`, `close`,
`getpid`, `usleep` and `clock_gettime` themselves, which is a library that
works on two systems out of three. They call `wolang_fd_*`,
`wolang_process_id`, `wolang_sleep_micros` and `wolang_clock` now, and
each system answers in the runtime. It also fixed a Linux bug nobody had
seen: clock 6 is `CLOCK_MONOTONIC` on Darwin and `CLOCK_MONOTONIC_COARSE`
on Linux, so `Now()` there ticked in milliseconds. `std.env.System()`
says which system it is, and the tests that start programs ask it rather
than assuming `sh`, `cat` and `/tmp`.

**D204. Time zones on Windows come from the system's ICU.** The C library
there understands `EST5EDT` and nothing with a slash in it, so
`Europe/Kyiv` was UTC. Windows has shipped ICU as `icu.dll` since 2019;
it is loaded when asked for, and the C library is the fallback when it is
missing. Still the system's database rather than one of ours (D176).

**D205. What a CI that never ran was hiding.** `fmt --check lib` failed
on every system, because a directory that is not a package was read as a
file; forty-odd files had drifted from the formatter since. `wolang.h`,
the header a C host includes, included `<cstdint>`, which C does not
have. The documentation check wrote a Windows path into TOML, where its
backslashes are escapes. The driver looked for `libwolang_platform.so`
on Windows. Each is fixed, and the Windows job now runs everything the
Linux one does except the ahead-of-time tier, whose linking is not wired
there yet.

**D206. A counted class lets go of what it holds.** D193 counted the
handle and stopped there, and a counter that measures live objects
(`wolang_objects_live`) showed how far that was from enough: an object
that died kept every object its fields pointed at, a field written twice
kept the first value, a `var` assigned twice kept the first object, a
`Leaf?` was not counted at all, and handing a local to any function
leaked it -- the use cleared the local's flag as if it were a move, and a
class with no `deinit` is not one.

The rules now, in every tier, because all of it is lowering:

- A class with a `deinit` moves (D87); one without is counted, and
  handing it on is another holder. Only what moves has a flag a use can
  clear.
- Every class has a `$drop` function: the destructor, a release of every
  field that holds a class or a `T?` of one, the storage. A function and
  not inline code, because `class Node { var next: Node? }` would expand
  forever.
- A store into an object's field, a module-level `var` or a local slot
  takes a reference to what goes in and lets go of what was there --
  after the store, so that `a.x = a.x` does not free what it is keeping.
- A constructor and a binding take a reference to what they are given;
  a call borrows, so a reference made only to be passed is released
  when the call returns, and one made by an expression statement is
  released on the spot.
- `T?` of a class is counted through its tag.

Cycles still leak. `weak` is what breaks them, and it is next.


**D207. `weak var` breaks a cycle.** A parent that holds its child and a
child that holds its parent is the shape of every scene graph, and
counting alone never frees it. A field marked `weak` holds a `C?` of a
class without keeping it alive: reading it asks the runtime whether the
object is still there and answers a counted reference or `null`.

The second word in front of every object -- sixteen bytes were reserved
for one count (D193) -- is the weak count, plus one for all the strong
references together, which is the arrangement Rust's `Arc` uses. The
object goes when the strong count reaches zero; its storage goes when
the weak count does, so a weak field never points at freed memory. The
upgrade is a compare-and-swap that will not raise a count from zero.

`weak` is contextual, like `derived`, and only allowed on a `C?` field
of a class (`E0137`): a weak local would be `null` by the next line, and
a weak field of a value would be copied without being counted.

Found on the way: `p?.x` on a class read the field out of the handle as
if the handle were the object, which is a wrong answer in machine code
and a trap in the reference tier. It reads through the address now.

**D208. Ahead of time on Windows, through `LLVM-C.dll`.** LLVM's Windows
installer ships the DLL that is all of LLVM's C API and nothing to
compile against it. `tools/package/fetch_llvm.py` takes the headers, the
import library and that DLL out of the official release archive into
`build/llvm-sdk`, CMake finds them there, and the DLL is copied beside
the programs that load it. One DLL rather than LLVM's static libraries:
no C runtime to agree with, every target included, and the C API is
the only part this project uses (D98).

`wolang build` links with `clang-cl` and the same C runtime the runtime
archive was built against; `LinkCommand` is the one place that says how,
for the driver, the fuzzer and the tests alike.

Pinned to LLVM 22.1.8, not the 23 the compiler itself is built with.
The differential fuzzer found it in the first sixty programs: LLVM 23's
interprocedural constant propagation folds `x != 6 % x` to false when
`x` comes from a call and is known not to be zero. The IR before the
pass is correct and LLVM 18 and 22 agree with the interpreter; 23.1.3
and 23.1.4 do not. `WOLANG_AOT_IR=<file>` writes the module before and
after the optimizer, and `WOLANG_LLVM_ARGS` hands LLVM its own flags --
`-opt-bisect-limit` is what named the pass.

**D209. A class inside a value is counted too, and classes do not move.**
A struct, an enum and a `T?` or `T!` of either that holds a class is a
holder of it: a copy retains every class it carries, its end releases
them -- `Pair$drop` for a struct, a switch on the tag for an enum -- and
`Free` on a slice releases its elements first, because storage starts as
zeroes and zeroes are an empty reference of every counted type.

That only adds up if nothing moves a class. D87 moved a type with a
`deinit`; for a class that made handing a `List` out of an enum's payload
a move out of something borrowed, and the enum and the caller both freed
it. A class is counted now whether or not it has a `deinit`, and the
`deinit` runs when the last reference goes. Moves are left to values
with a `deinit` and to owned strings.

What it took to agree:

- A call's result is fresh -- it owns what it holds -- and so are `new`,
  a struct or variant built here, and the answer of `??`, which takes the
  value of a fresh optional and retains that of a borrowed one. Fresh is
  per function: an instruction id means nothing in another one.
- The subject of a `match` that is fresh is held until the scope ends,
  so every payload bound from it stays valid in every arm.
- A generic method lowered for `List<Field>` asks about `Field`, not
  `T`, and asks the file that declared `Field`, not the one that
  declared `List`.
- A `match` arm's answer is coerced to the type of the whole `match`;
  `Leaf` where `Leaf?` was expected was a verifier error nobody had hit.

`WOLANG_TRACE_RC=1` prints every allocation, retain, release and free,
which is how the last of these were found. What still leaks: a `text`
inside a document value is a view of bytes nobody owns, which is a
question about `std.doc`, not about counting.

**D210. The documentation lives on mowoengine.com.** Beside the engine's
guides, in the same engine-docs repository the site pins:
`tools/package/make_site_docs.py` turns `docs/` and the API reference
`wolang doc` writes into MDX under `Wolang/Site/`, and the site renders
them at `/docs/wolang/` and `/docs/wolang-api/`. The guides stay Markdown
here, next to the code, and every example in them is still compiled on
each commit; the script only escapes what MDX would read as an
expression and turns links between files into links between pages.
`.github/workflows/docs.yml` publishes on every push to `alpha`, the way
the engine's docs workflow does.

`wolang doc` writes one package to one file. The CI step that asked it
for three directories and a folder of per-module files had never run.

**D211. TLS lives in the runtime, and on Windows it is Schannel.**
`lib/tls` declared OpenSSL's functions itself and linked `ssl` and
`crypto`, which is a package that works where OpenSSL is installed and
nowhere else -- not on Windows, and not in a binary built ahead of time
on a machine that has no OpenSSL headers. Four runtime calls replace it:
`wolang_tls_open`, `_read`, `_write` and `_close`, over a runtime socket.

Windows answers them with Schannel through SSPI, the TLS every Windows
program already trusts, with the system's roots and its own name check.
Elsewhere OpenSSL is loaded with `dlopen` the first time a stream opens,
so a program that never speaks TLS needs no OpenSSL at all, and the
package needs no `[link]` table. Still the system's TLS rather than ours
(D150); only where it is reached from moved.

Checked on both systems against a real server and against expired,
self-signed and wrong-name certificates, which all refuse.

**D212. An ability can leave a type to whoever implements it.** `type
Item` in an ability, `type Item = int` in `extend T as Ability`, and
`C.Item` in generic code. What it bought straight away: `for` over a `C:
Iterable`, which is a function that walks any collection -- `std.iter` is
`Iterator` and `Iterable`, and `List`, `Deque`, `Map`, `Split` and `Chars`
implement them. D96's three names still work for a type that says nothing
more.

`C.Walker.Item` is a type of its own kind, a projection: an owner and a
name. Inside the generic body it stays one, so `walker.Value() + 1` is an
error when nothing said `Item` is a number. Once the owner is concrete --
after a call infers `C`, and in every copy monomorphization makes -- it
is whatever the implementation answered, read off the member table of the
file that declared the type. Bare `Item` inside the ability is a
projection of the ability itself, which a use rewrites onto the type it
is used on.

`extend<T> Box<T>` did not work at all before this: its members were kept
under `Box<T>` in the extension's own `T`, which no `Box<int>` ever
equals, and lowering named its methods `Box<<error>>`. They belong to
`Box` now, rewritten into the declaration's parameters, and a method of
one is a generic recipe like a method declared inside the class.

`type Count` with nothing after it is `E0138` outside an ability.

**D213. Objects cross the embedding boundary, and the host counts its
own.** An engine's actor is made and freed by the engine; a script
holding one has to say so without owning it. `extern class Actor { fun
Hurt(amount: s32) }` is a handle like any class, with three differences:
retaining and releasing one calls the host through
`wolang_host_retain`/`_release` instead of the header Wolang's own objects
carry, a `weak var` of one asks the host's `alive` when it is read rather
than keeping a weak count, and a method is the host's `Actor_Hurt`, taking
the object first. The runtime finds the callbacks by a tag both sides
compute -- the FNV-1a hash of the class's name -- so the compiled tiers
pass a constant and need no table of their own. Construction and fields
are `E0139`: the data lives on the host's side, where the engine's own
code reads it.

The other direction is `kWolangObject`, a Wolang object the host holds a
reference to. Releasing the last one has to run the class's `$drop`,
which used to exist only when something in the program released one, so
every non-generic class a file declares gets one now. Methods and
`$drop`s are found by name rather than by declaration, because a `$drop`
has none. Two counting gaps showed up on the way: `held = null` did not
release what was there, because the literal had no counted type, and an
operand made only to be compared -- `tracker.seen != null` upgrades a weak
field into a reference of its own -- was never let go.

**D214. A vm takes one call at a time, from any thread.** Every entry
point that touches a vm holds its lock, a recursive one, so a host
function that calls back into the vm it was called from does not
deadlock itself. The alternative was to leave it to the host, which is
what "not thread-safe" said before, and every engine would have written
the same mutex. Parallelism is separate vms, which were already
independent: the compiled tier's context belongs to its vm, the counts
on objects are atomic, and the host's class table has its own lock.

That last part was not true when it was first written: module-level
`var`s lived in one table for the whole process, keyed by file and
declaration, so two vms holding the same program shared them. They live
in a global space now, one for the process and one per vm, and a call
into a vm enters its space for the thread making it. Threads a script
starts enter the space of the thread that started them.

**D215. A reload keeps the program's state.** `wolang_vm_reload` used to
throw everything away, which is a restart and not a reload. It builds the
new program in a new global space, then carries each module-level `var`
across by name when the type has the same *shape* -- the name, and for a
declared type every stored field by name and shape, all the way down. A
value is moved, not copied: the old program's globals are never read or
released again. A class that gained or lost fields keeps the fields that
are still there, written into the object the new initializer made. A
class with virtual methods is never carried, because its descriptor
belongs to the code that made it. Whatever starts again is listed in
`wolang_vm_notes`.

The old program is retired rather than freed -- its database, its code
and its globals stay until the vm is destroyed. A carried value can point
into it: a `text` literal is bytes in the old program, and freeing it
would leave the new program holding a dangling string. Function handles
name their function instead of pointing at it, so they survive.

**D216. The debugger is the interpreter.** `docs/debugging.md` used to
say a Wolang debug adapter would be a worse lldb. That was true for a
binary, and no help at all for a script inside an engine, which has no
binary of its own and runs in the JIT. Teaching three code generators to
stop at a line -- deoptimization, patchable breakpoints, register maps
for every local -- is the expensive way. The interpreter already runs
every program the compiled tiers do, and the differential fuzzer keeps
it running them the same way, so a program being debugged runs there.

Lowering records every local the source named: where its value is (the
slot of a `var`, the value of a `const` or a parameter) and the range of
instructions it is in scope for. The debugger runs the IR as lowering
left it, before any pass, so that table is exact. The interpreter asks
the debugger whenever it reaches a new place in the source, and the
session decides by line, by depth and by what the editor last asked for
-- a call that returns lands in the middle of its caller's line, which
is not a new line, so a breakpoint there does not fire twice.

`wolang dap` speaks the protocol over standard input and output for
`launch`. An embedding host calls `wolang_vm_debug_listen` for `attach`,
and while an editor is attached every call into that vm runs on the
interpreter; when it goes, the compiled tier is back. The session sits
behind a transport it does not know about, which is how one class serves
a pipe, a socket and a unit test.

**D217. A capability is stated where the door is, and a mod's grant is the
host's.** `capabilities = ""` looked like a sandbox and was not one.
`unsafe` is absorbed by a declaration that states it (the rule that makes
a safe wrapper possible), and `std.fs.Open` stated `uses unsafe` and
nothing else -- so a package granted nothing could open files through it,
and the compiler said nothing. Reviewing two hundred signatures by hand
would have fixed it until the next one.

The capability is stated on the `extern` declaration instead: the
primitive that touches the world says which part, `wolang_fd_read` `uses
io`, `wolang_socket_connect` `uses net`, `wolang_clock` `uses time`,
`wolang_entropy` `uses rand`. `unsafe` is still absorbed and these are
not, so they propagate through every wrapper, and the compiler found the
two hundred signatures that had to say so (`E0701`, `E0704`). A new
`std` function cannot forget, because it would not compile.

The other half is who writes the grant. A mod's manifest is the mod's,
so `wolang_vm_load_mod` applies the host's grant to the package before it
is checked -- narrowed by the manifest when it lists capabilities, never
widened -- with `unsafe` denied and the `[link]` table dropped. And a
sandboxed package cannot declare `extern` (`E0705`): calling one would
need `unsafe`, but an `extern class` method has no body and no effects of
its own, so `extern class wolang { fun system(...) }` bound
`wolang_system` without either.

**D218. Engine bindings are generated from the engine's reflection.**
MowoEngine's header tool already writes everything `MCLASS`, `MFUNCTION`
and `MPROPERTY` declare as JSON, for the editor and the Visual Script
palette. `wolang bind --reflection` reads the same files and writes a
Wolang package and the C++ glue that answers it, so the bindings are as
complete as the reflection and nobody keeps two lists in step.

The C boundary takes numbers, `bool` and pointers, and the engine's API is
`Vec3`s and strings. A method that needs either gets a raw form -- `out:
*mut Vec3`, `name: *u8, nameLength: u64` -- and a method in an `extend`
block that turns script values into it. That needed one rule changed: a
method of an `extern class` used to be the host's whatever it was, and
now it is the host's only when it has no body. Properties are reached by
the offset the engine's own reflection records, asked for once per
property, because an `MPROPERTY` is usually private and the glue is not
a friend. A `std::string` answer is left out until the boundary has a way
to hand bytes back that the script then owns.

Checked against the engine as it is: 165 classes, 734 functions and 256
properties, a generated package that checks clean, and glue that compiles
against the engine's headers with the engine's own flags.

`need mowo.{World}` for a `uses World` clause was reported as unused,
because only expressions counted as uses. An effect a `uses` clause names
counts now.

**D219. A frame's calls are words, and a shipped script is a library.**
The first measurement of a host ticking a thousand actors through a
script said 61× slower than C++, and the script was not the reason: each
call looked the function up by name, boxed its arguments into a vector,
found the compiled body by searching every function, and decoded the
answer through a layout. A `WolangFunction` now prepares itself once per
program -- the compiled entry and how each argument goes into a word --
and a call is the words on the stack and a jump into the stub, the same
way the scheduler resumes a task. A reload makes the preparation stale by
giving the program a new generation. That took the dev tier to 11×.

A shipped game does not run the dev tier. `wolang build --library actors`
compiles a package into an object a host links: no `main`, an
`actors_start` that registers classes and starts the module's `var`s, and
every offered function under `actors_<Name>`, an alias so the module's
own calls are untouched. Through that, the same `Tick` is 1.7× the C++ --
which calls virtual methods directly where the script goes through C
functions that call them.
