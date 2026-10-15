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
