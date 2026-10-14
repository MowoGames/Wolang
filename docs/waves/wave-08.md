# Wave 8 — the dev tier

**Status:** complete. 549 unit tests, all passing in Debug, sanitizers and
Release.

## Scope

Compile the IR to machine code. The exit criterion was gameplay
microbenchmarks within 3x of C++ and generated code that agrees with the
interpreter on every test input.

```
$ wolang bench benchmarks/gameplay.wo
@BenchFibonacci                1.600 ms   result 196418
@BenchSumLoop                  4.576 ms   result 8999994
@BenchVectorDot                3.430 ms   result 7000000
@BenchArraySum                 4.066 ms   result 7750000

$ wolang test lib/core
[]ok ComparesInOrder (both tiers)
[]ok ClampsToTheRange (both tiers)
```

The second of those is the wave's real result: the same functions, compiled
and interpreted, reaching the same answers.

## What was built

### The assembler — `include/wolang/codegen/arm64`

Every A64 instruction is four bytes, which is what makes copy-and-patch
natural on this target (D5): a stencil is a word and patching an operand is
writing a bit field. The assembler is that patch step, with labels and
fixups for branches.

Its encodings are **checked against a system assembler's**. Ninety-two
instructions were written out in both, assembled with `clang`, and compared
word for word; the test keeps a representative subset. An encoder that is
almost right produces code that almost works, and nothing further up the
stack can tell the difference — three of the test's own expectations were
wrong, written from memory, and the comparison is what found them.

### Layout — `include/wolang/codegen/layout.h`

The interpreter never needed this: its values carry their own shape. A code
generator has to decide once how many bytes a struct takes and where each
field starts. Its own component rather than a detail of the emitter, because
the FFI in wave 9 needs the same answers.

### Register allocation — `include/wolang/codegen/reg_alloc.h`

Linear scan from the start (D39), in its simple form: one interval per value
with no holes, no coalescing, no live range splitting. Two details are load
bearing.

**Only callee-saved registers are handed out** (D44). Nine integer registers
and eight floating point ones, and in exchange a call needs no spilling
around it — every value the allocator placed survives a call because the
callee is obliged to restore it.

**An interval ends one past its last use.** A value's register is therefore
never reused for the result of the instruction that consumed it, which
matters because an overflow check reads its operands *again after* writing
the result. Pinning that in the interval arithmetic removes a whole family of
bugs that would otherwise appear only in overflowing programs.

### The emitter — `src/codegen/arm64/emitter.cc`

Scalars live in registers; aggregates live in the frame and are passed by
pointer (D45). Integers narrower than a word are kept extended to 32 bits, so
a comparison answers the question the declared width asked, and every
operation that can leave a wider result re-extends and checks.

A trap is a check before the operation branching to a three-word stub (D43),
and each site carries the kind, the place and the message the interpreter
would have produced — so the two tiers' outcomes compare in full and not just
on the kind.

### Unwinding and symbolization

Every compiled function builds a frame record, even a leaf (D49). That is all
the unwind information this tier needs: a trap walks the chain of frame
pointers, symbolizes each return address against the code map, and stops at
the trampoline the host entered through. `CodegenTest.NamesTheCallersOfATrap`
divides by zero three calls deep and gets three named frames.

### Monomorphization

The debt from wave 4, and it turned out to be the difference between generics
that type check and generics that run. Before it, every generic call produced
IR the verifier rejected — `call @Identity` passing an `s32` where a `T` was
declared.

A generic function is now never lowered (D42). The checker records what each
generic call was inferred to be, and lowering emits one specialization per
instantiation: `Identity<s32>`, `Identity<f32>`. A generic body calling
another generic passes its own type parameters along, so the inner
instantiation becomes concrete only when the outer one does — which is why
specializations come off a worklist rather than a second pass.

### Inlining, and what it needed

The debt from wave 6. The splice is where the IR's id design pays off: the
**call instruction becomes the parameter** of the block control returns to,
so every operand that read the call's result reads the parameter instead and
nothing is rewritten.

It needed two things to pay off, and both were missing:

- **Block merging.** `SimplifyControlFlow` documented it and did not do it.
  Merging a block into its one predecessor means substituting the block's
  parameters with the values the jump carried — which is also what turns an
  inlined body back into a straight line. `ClampInt`, two calls deep, is now
  two compares and no calls.
- **Slot promotion for a slot written on several paths**, the other wave 6
  debt. This is the big one: a loop counter was making a round trip through
  the frame every iteration, and the loop ran at a third of its speed. The
  general case is SSA construction, inserting a block parameter where a value
  depends on which edge arrived — exactly what the block-parameter form was
  chosen for.

### Effect rows, actually populated

Wave 6 decided effect rows belong in the IR. Lowering never filled them in,
so every function's row read as "does nothing observable" — which, had
anything started trusting it, would have been a miscompile waiting. Rows now
come from the module-wide inference, prepared by the lowering query and
handed over, with a second bit (`effects_known`) because an empty row is an
assertion (D38) and an absent one is not.

What the row still does not license is the removal it was kept for: it says
nothing about *trapping*, and deleting a call that would have faulted turns a
faulting program into one that does not — the same argument that makes
arithmetic impure (D15c). Named here so the next person does not read the row
as permission.

### The driver

| Command | Does |
|---|---|
| `wolang asm <paths>` | what the tier compiled, and for each function it did not, why |
| `wolang bench <paths>` | runs and times every `@Bench`, best of five |
| `wolang test <paths>` | now runs every `@Test` in **both** tiers and compares |

`wolang run` stays on the interpreter (D51): any interesting `main` prints,
and printing goes through an intrinsic this tier cannot call until the FFI.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| A trap is a check, not a signal handler (D43) | portable and predictable; range analysis can remove it later, and no target lacks the mechanism |
| Only callee-saved registers are allocated (D44) | a call then needs no spilling around it |
| An aggregate lives in the frame, passed by pointer (D45) | one rule for every aggregate however it was produced |
| A trap reports by non-local exit (D46) | the path that does not fault pays nothing |
| `TrapKind` belongs to the IR (D47) | both tiers must name a fault identically to be comparable |
| A trap message says only what both tiers know (D48) | a dynamic value cannot be in a string built while compiling |
| Every function builds a frame record (D49) | a stack walk for two instructions, with no table |
| The tier compiles what it can and says what it did not (D50) | the interpreter is always there; a guess would be undebuggable |
| A generic is never lowered; one copy per instantiation (D42) | otherwise the IR carries a `T` every back end needs an answer for |
| `as` is numeric, and narrowing traps (D41) | overflow traps everywhere else; a quiet truncation would be the exception |

## Bugs this wave found

- **A non-leaf function corrupted its return address.** The frame record was
  built in the entry trampoline and not in the function, so a `bl` from a
  compiled body overwrote the link register and the outer `ret` returned into
  the middle of its own caller. Found by `Fib`, which trapped on an overflow
  that could not happen.
- **The trap state lived in automatic storage across a `longjmp`.** Debug and
  sanitizer builds were green; the **release** build reported every trap as no
  trap at all. An object changed between `setjmp` and `longjmp` has an
  indeterminate value afterwards unless it is volatile, so the optimizer
  folded the trap kind back to the zero it was initialized with — correctly.
  The context moved into the heap object (D46).
- **`as` did nothing.** The interpreter returned its operand unchanged, so
  `n as s64` kept its width and `x as int` stayed a float; lowering did not
  emit the cast at all; and the checker accepted `s as int` on a string.
  Fixing it needed a decision about what `as` means (D41) and a new
  diagnostic (`E0124`).
- **A widening cast kept the bit pattern instead of the value.** `-5 as s64`
  became 4294967291. Found by the differential check, which is exactly the
  kind of thing it is for: the machine code was right and the reference
  implementation was wrong.
- **A `var x: int` with no initializer had an `<error>`-typed slot.** Nothing
  noticed while the slot stayed in memory; slot promotion put the value on an
  edge and the verifier compared the types.
- **Three of the assembler test's own expectations were wrong**, written from
  memory rather than taken from an assembler. That is the argument for
  checking encodings against an outside answer rather than against one's
  reading of the manual.

The first two are the interesting ones: both were invisible in the
configuration most work happens in, and both were found by running all three
presets rather than by a test anyone thought to write.

## The benchmarks

`benchmarks/run.sh` runs four gameplay kernels in Wolang and the same four in
C++, and prints two ratios:

| Kernel | Wolang | C++ `-O2` | ratio | C++ scalar | ratio |
|---|---|---|---|---|---|
| `BenchFibonacci` | 1.60 ms | 1.25 ms | 1.28x | 1.31 ms | 1.22x |
| `BenchSumLoop` | 4.58 ms | 1.32 ms | 3.48x | 4.12 ms | 1.11x |
| `BenchVectorDot` | 3.43 ms | 1.52 ms | 2.25x | 1.52 ms | 2.25x |
| `BenchArraySum` | 4.07 ms | 1.60 ms | 2.54x | 1.60 ms | 2.54x |

Three of four are within 3x of a full `-O2` build, which was the exit
criterion. The fourth is a counting loop that clang turns into vector code; a
scalar tier is not within 3x of vector code and is not trying to be, which is
why the second column exists — against scalar C++ the same kernel is 1.11x.
Vectorization belongs to the optimizing tier, and the honest way to say that
is to measure both.

Before slot promotion the same three were 3.2x, 3.2x and 4.1x. A loop counter
in memory was the whole difference.

## Things worth remembering

- **The differential check paid for itself twice more.** It found the
  interpreter's widening cast, and it is what made rewriting slot promotion
  into full SSA construction a half-hour change rather than a week of
  suspicion: every interpreter and codegen test runs the optimized and
  unoptimized IR and compares, so a rename bug would have shown up as a
  disagreement in a test that was already there.
- **An interval that ends one past its last use** is the cheapest possible fix
  for "the result register must not be an operand register", and it is worth
  knowing why it is there before shortening it.
- The emitter keeps integers canonically extended to 32 bits, *not* to 64: a
  32-bit instruction zeroes the top half of its register, so a 64-bit
  canonical form would be destroyed by every narrow operation. Anything that
  wants a 64-bit view extends explicitly.
- `wolang asm` is the first thing to run when something is slower than
  expected. "Why is this slow" has an answer in the form of a reason per
  function.

## Deferred to later waves

- **Strength reduction for a constant divisor.** The dev tier emits a hardware
  divide where every optimizing compiler emits a multiply-high; it needs a
  multiply-high opcode in the IR, which the assembler already has the
  instructions for.
- **Vectorization**, and with it the remaining gap on a counting loop.
- **Splitting a critical edge**, which is the one case slot promotion
  declines: a block that needs a parameter whose predecessor ends in a branch
  rather than a jump.
- **Cross-file monomorphization.** A generic instantiated from another file
  gets a declaration and no body, because lowering is per file; whole-program
  instantiation belongs with the linking story.
- **Strings, slices and closures in compiled code.** Each wants the heap, and
  the heap wants wave 9's memory rules.
- **Constructing an enum variant is not lowered** — it produces `undef`, which
  was true before this wave and stays true: matching on one works, building
  one does not.
- Spill slots are never reused, and an aggregate temporary always gets its own
  frame area. Both are the kind of waste a frame layout pass removes.

## Next

Wave 9: the FFI and the memory rules — `extern` declarations through the C
ABI, the real `std` writing through the platform layer, and `E0121`–`E0123`,
which is what makes a slot read before it is written an error rather than a
difference between tiers. See `STATUS.md`.
