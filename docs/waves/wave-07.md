# Wave 7 — the language runs

**Status:** complete. 513 unit tests, all passing in Debug, sanitizers and
Release.

## Scope

Make Wolang executable. The exit criterion was that `examples/hello.wo` runs
and `@Test` functions execute — both of which now happen:

```
$ wolang run examples/hello.wo
hello, world

$ wolang test lib/core
[]ok ComparesInOrder
[]ok ClampsToTheRange

2 tests run, 0 failed
```

The second of those is Wolang written in Wolang, type checked, lowered, and
executed, with its own tests passing.

## What was built

### The interpreter — `include/wolang/interp`

The reference semantics (D4). Four decisions shape it.

**A trap is a defined outcome, not a crash.** Division by zero, overflow, an
index outside an array, reading an absent optional, reaching an
`unreachable`, running out of steps, calling something with no body: each
stops execution with a named kind and a location. That is what makes "this
program faults" something a later tier can be compared against, rather than
a difference in how two tiers happen to misbehave.

**Overflow traps rather than wraps.** A language that wraps silently turns an
arithmetic bug into a wrong answer somewhere else entirely. This decision
reached further than expected — see below.

**The step limit belongs to the run, not the program.** `comptime` runs
inside the compiler and gets a million steps; a program run gets ten
million. Either way an endless loop stops being the compiler's problem.

**Values are simple rather than compact.** An integer is 64 bits with its
declared width remembered separately, so wrapping happens in exactly one
place (`MaskToWidth`). An f32 rounds through a `float` after every
operation — doing f32 arithmetic at double precision would make this tier
disagree with every other one, which is the one thing a reference
implementation may not do.

### Differential execution

`wolang run` executes the program **twice** — on the IR as lowering left it,
and after the passes — and compares the result, the trap, and what was
printed. A difference is a miscompile, the most serious bug this compiler can
have, so it is checked on every run rather than in a nightly job. Every
interpreter test does the same through its fixture.

This paid for itself immediately: it found dead-code elimination deleting a
division by zero.

### `@Test` and `comptime`

`wolang test` runs every function carrying `@Test`. A test passes when it
returns without trapping and does not return `false`. Tests run on the
unoptimized IR, because the reference semantics are what a test is testing.

`comptime` functions with no parameters are evaluated during compilation, in
the lowering query, and a trap becomes `E0901` with the function's span. That
placement is deliberate: evaluating during *checking* would need
`TypeCheck` → `LoweredFile` → `TypeCheck`, and the query engine would be
right to call that a cycle. What this cannot yet do is use a `comptime`
result as a type or an array length; that needs the deeper integration and a
later wave.

### The platform floor

A body-less declaration whose name matches an intrinsic is implemented by the
host: `Print`, `ConcatStr`, `IntToStr`, `FloatToStr`, `BoolToStr`. The list
is deliberately tiny — everything the language can say about itself is
written in Wolang.

`lib/std/src/io` declares them in Wolang, with no bodies, which is what makes
the floor visible in the source rather than hidden in the compiler.

### Interpolated strings

`"n={n} f={f}"` lowers to conversions and concatenations of those
intrinsics, picking the conversion by each piece's type. A piece whose type
has no conversion yet becomes `<?>` rather than an error, because the real
answer is a formatting ability and that arrives with the rest of `core`.

### `lib/core`, in Wolang

`lib/core/src/cmp.wo`: `Ordering`, `CompareInt`, `MinInt`, `MaxInt`,
`ClampInt`, and two `@Test` functions. Its manifest sets
`capabilities = ""`, so the package is granted nothing — the empty grant is
what *enforces* "core allocates nothing and touches nothing" rather than
leaving it to discipline.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| A trap is a defined outcome with a kind and a location | a later tier has to be comparable against it |
| Overflow traps rather than wrapping | a wrap turns an arithmetic bug into a wrong answer elsewhere |
| Arithmetic is therefore **not** pure (D15c) | removing an unused `a + b` would turn a faulting program into one that does not fault |
| An owned `Text` is accepted where `text` is wanted (D15b) | an implicit borrow, safe because references are second class; without it, printing an interpolated string would not type check |
| The step limit is a property of the run | `comptime` and a program run want different budgets |
| `comptime` is evaluated in the lowering query | evaluating during checking would be a query cycle |
| Differential execution on every run | a miscompile is too serious to find in a nightly job |

## Bugs this wave found

- **Dead-code elimination removed a division by zero.** Found by the
  differential check on its first real test. The fix was to accept that in a
  language where overflow and division trap, arithmetic is not pure — which
  makes `PassTest.KeepsUnusedArithmeticBecauseItCanTrap` a statement about
  the language rather than a limitation of the pass.
- **A value flowing into an optional position was not wrapped.** `return
  Pawn(h)` from a `-> Pawn?` function built a struct where an optional was
  expected. The cause was subtle: the checker records the type the *context*
  wanted, so the expression already read as `Pawn?` and nothing looked wrong.
  Lowering now builds at the type the instruction actually produces and
  wraps to reach the recorded one.
- **An endless loop's exit block was unreachable.** `loop { }` with no
  `break` produced a block nothing jumps to. The exit is now created on
  demand, by the first `break` that needs it, so a loop nothing leaves has no
  exit at all.
- **Array literals and indexing were not lowered**, so `xs[i]` reported
  "undefined value" instead of a bounds trap.
- **`wolang run` printed nothing.** The output was collected into a sink the
  driver passed as null.

## Things worth remembering

- The checker's `Adapt` returns the *expected* type when a value widens into
  an optional, so `TypeOf(expr)` is the context's type and not the
  expression's. Lowering has to compute what an instruction actually
  produces; reading the recorded type is what hid the missing wrap.
- `Output` is collected rather than written straight to stdout, so a test can
  read what a program printed — and so the differential check can compare it.
  What a program prints is part of what it does.
- `lib/core`'s `capabilities = ""` is load-bearing. Adding anything that
  allocates or touches the outside will fail to compile, which is the point.

## Deferred to later waves

- A `comptime` function with arguments, evaluated at its call site, and a
  `comptime` value used as a type or an array length.
- Formatting abilities in `core`, which is what turns `<?>` into a real
  conversion for a struct or an enum.
- `for` and `foreach` are still not lowered; iterators need the ability
  machinery.
- `T!` and error propagation with `?` are checked but not lowered.
- The rest of `core`: slices, iterators, math. Each wants a language feature
  that is not finished — `core` grows as the language does, which is the
  point of writing it in Wolang.
- `Print` goes through an interpreter sink. A real `std` writes through the
  C++ platform layer, which needs the FFI from wave 9.

## Next

Wave 8: the dev tier — copy-and-patch code generation, arm64 first. The IR
and the interpreter together are what make a code generator checkable: every
program it compiles has an answer to compare against. See `STATUS.md`.
