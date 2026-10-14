# Wave 10 — suspension, and a second machine

**Status:** concurrency, strings, memory and the server are in. The core of
the wave is complete — the `async` effect, the task
transform, `spawn`, a scheduler, and `std.tasks`. Machines (`machine` /
`state`), signals and `derived`, and `wolang-http` as a design driver are
not built and are carried forward. The x86-64 backend, which the roadmap
put later, landed here instead; the end of this report says why.
608 unit tests, all passing in Debug, sanitizers and Release, and all of
them passing again when the whole compiler is built for x86-64.

## Scope

Concurrency. A `task` that suspends, a `wait` that resumes it, a `spawn`
that hands it to a scheduler, and the question the wave exists to answer:
how much does a waiting coroutine cost?

```
$ wolang bench benchmarks/tasks.wo
@BenchManyTasks               48.700 ms   result 500000
@BenchOneTaskManyRounds       92.065 ms   result 1000001
```

A hundred thousand tasks, all waiting at once, advanced five times each:
500,000 resume steps in 48.7 ms, and **two megabytes of frames** — twenty
bytes a task. The same work on threads or on stackful coroutines would be
four hundred megabytes of stacks. That is the whole argument for being
stackless, and it is the wave's exit criterion met.

## What was built

### Suspension is an effect

`task` in a signature declares the `async` effect, the same way `uses io`
declares `io`. Nothing new was needed to make that work, which is the
return on having built effects as a row in wave 6: `comptime` and `@Hot`
forbid the effect and so reject a task without a special case, and an
effect that must be declared is an effect that shows up in a signature
where a reader will see it.

Five diagnostics, and each one is a consequence rather than a rule:

| | |
| --- | --- |
| `E0301` | `wait` outside a task — only something that can suspend may |
| `E0302` | a task called without `wait` or `spawn` — a task that nobody resumes does nothing |
| `E0303` | `wait` is not the whole of its expression |
| `E0304` | a task waits on itself |
| `E0305` | `wait` on a task from another module |

`spawn` is deliberately **not** `async`: the spawner does not suspend, so
it does not need the effect. It does inherit every *other* effect of what
it spawns, through a fixpoint over the call graph — spawning something that
writes to a file is writing to a file, whoever gets there first.

### A task is a struct and a function

The transform in `ir/tasks.cc` runs before anything executes, which is the
decision the whole wave rests on. Everything that outlives a suspension
goes into a **frame**: the state to resume at, the parameters, the result,
one field per slot the body keeps, one field per `wait` for the callee's
frame. The body becomes `F$resume(frame) -> bool` with a switch over where
it left off; `F` itself stays as the driver that allocates a frame, loops,
and returns the result.

After the transform there are no tasks — only functions and structs. So the
interpreter needed no change, the emitters needed no change, and the
differential check between the tiers covers coroutines for free. It also
means the optimizing passes see a state machine as ordinary code: the
verifier caught two real bugs in the transform (every frame address has to
be taken in the block that dominates all the states, and a block's
parameters have to be copied) that a hand-written test would not have
thought to look for.

The callee's frame lives *inside* the caller's, so a `wait` allocates
nothing and recursion is rejected rather than quietly needing an unbounded
frame. A task with no body — `std.tasks.Yield` — is the suspension
primitive, and the compiler writes its machine.

### One scheduler, two tiers

`spawn` allocates a frame, writes the arguments into it, and hands the
**kind and the frame** to the scheduler in `runtime/`. Not a function
pointer: the transform generates one `wolang$dispatch(kind, frame)` per
module, so the scheduler can advance a task without knowing which tier is
running it. Round robin, cooperative, single threaded.

The first version of that path cost 330 ns a step, nearly all of it the
host boundary: every resume marshalled its arguments, boxed a result and
allocated. A scheduler step is the hot path of every program that has tasks
in it, so it got its own door — `CompiledModule::ResumeTask`, which
allocates nothing — and the step went to 97 ns. What is left is the
landing pad set up per step; hoisting it out of the loop is the next
obvious win.

### Across files

Two things turned out to be the same problem. Lowering is per file, so a
call into another file is a declaration with no body, and until this wave
the interpreter simply could not make one: a multi-file program was
interpretable only if nothing crossed a file. Now the interpreter is handed
every module of the program and resolves such a call by the declaration it
came from — which is exactly what the code generator links by, so the two
tiers agree by construction rather than by coincidence.

The same per-file limit is why `E0305` exists: a `wait` needs the frame
layout of what it waits on, and that is computed from a body this file
cannot see. The way out is a calling convention — generated `$framesize`,
`$start` and `$result` entry points, so a caller can fill a frame it cannot
lay out — and that belongs with the whole-program step cross-file
monomorphization needs. Until then `std.tasks.Join` runs the scheduler
rather than suspending, and says so in its own documentation.

One language rough edge came out of the renaming: `task` is a keyword now,
so a module could not be named `task`. The module is `std.tasks`. The
better fix is to let a keyword be a path segment after a `.`, where it
cannot be anything but a name; that is worth doing before something is
called `type` or `match`.

## The second machine

The roadmap has one backend until much later. It moved up because a
language with one backend has an untested seam, and because the request was
explicit.

An assembler and an emitter, no more: `codegen/x64`. The encodings were
compared against a system assembler's byte for byte, the same discipline
arm64 got — and once again the encoder was right and a hand-written
expectation was wrong. Four for four now.

What the port taught, which is mostly about what the first backend had
quietly assumed:

- **The seam counted in words.** Every A64 instruction is four bytes, so
  `CompiledFunction` held `uint32_t`s and a call fixup was a word index.
  An x86-64 instruction is one to fifteen bytes. The seam counts bytes now
  and each target patches a call displacement its own way, which is the
  one piece of placement that cannot be shared.
- **"Allocate only callee-saved registers" is a platform-dependent
  bargain.** It buys arm64 nine integer registers and eight floating
  point ones. On x86-64 it buys four integer registers and *zero*
  floating point ones, because this ABI has no callee-saved vector
  registers at all. So every float on x86-64 lives in a frame slot. It is
  slower and it is correct; spilling only what is live across a call is
  what fixes it, in the allocator rather than in either emitter.
- **Declining is a feature.** This machine's float-to-integer
  conversions are both signed, so a conversion involving a 64-bit
  unsigned integer is left to the interpreter rather than approximated.
  The tier that says what it cannot do can afford to be a tier.

And the part worth keeping: **the whole suite runs the x86-64 backend on
this arm64 machine**. The `x64` preset builds the compiler for x86-64, the
system's translator handles the code the dev tier generates at run time
along with everything else, and all 593 tests pass — including the
differential tests that run every program interpreted, optimized and
compiled and compare all three. A backend verified by execution rather than
by inspection, on hardware that is not the target. What that cannot do is
measure speed: a benchmark there times the translator.

## The server, and what writing it found

`wolang-http` is in `lib/http`: eight socket calls in `socket.wo` and
everything above them in Wolang. `examples/server` routes three paths over
it, and nothing in that file is unsafe -- the responsibility stops at the
floor, which is what `uses unsafe` on a declaration means.

```
$ wolang run examples/server
server: http://127.0.0.1:8080
http: listening

$ curl -s localhost:8080/echo
you said GET /echo, 81 bytes

20000 keep-alive requests in 0.648 s = 30848 req/s
```

Thirty thousand requests a second, which is where the measuring client
tops out: the same client against a threaded Python server reaches 30784,
so this is the floor of the measurement rather than the ceiling of the
server. One connection at a time, keep-alive, a four-kilobyte buffer and an
arena per request.

That number is the second thing the server produced. The first is a list
of holes it found, every one of which was a wrong answer rather than a
missing feature:

| what the server needed | what was actually there |
| --- | --- |
| a four-kilobyte buffer | the dev tier declined any frame over 4000 bytes, so the whole request path ran interpreted -- 25x slower |
| `sub sp, sp, x9` for a large frame | the shifted-register form of `sub` reads register 31 as *zero*, not the stack pointer, so the frame was never allocated and the first write faulted |
| `add x9, sp, x10` for an address in a large frame | the same, which is why the assembler now refuses the stack pointer in that form rather than encoding it wrongly |
| a buffer a callee fills | slot promotion replaced a load-before-store with an undefined value, so the buffer was undefined before the callee saw it |
| a slice of a local array | the conversion copied the array, so the callee filled a copy nobody could see |
| `bytes[i] = v` through a slice parameter | reported as an assignment to a `const` binding -- but a view writes the storage it views |
| `message.data + sent` to resume a partial write | no pointer arithmetic; now an element address, scaled by the tiers |
| a module-level `const` | lowered to an undefined value, silently |
| `message.len` | a field read of an owning string was treated as a *move*, so the next use was reported as a use after move |
| a string from a buffer the interpreter holds | the interpreter's values have no address, so handing one to C was impossible; now the slot is copied out to real memory for the call and copied back |

The interpreter's side of that last one is worth keeping: its values carry
their shape rather than living in memory, which is what makes it the
reference tier and also what makes a pointer into one meaningless. A buffer
handed to C is bounced through real memory, and a place names the frame it
belongs to so that a slice of a local array survives being passed to a
callee.

## What is carried forward

- **Machines** (`machine` / `state`), signals and `derived`. The syntax
  parses and the formatter handles it; nothing resolves it yet.
- **`wolang-http`** as the driver that was supposed to find the holes in
  the concurrency design. The benchmark found the one that mattered — the
  cost of a resume — but a server would find the ones about cancellation
  and lifetimes, which are still undesigned.
- **Hoisting the landing pad** out of the resume loop.
- **Cross-file `wait`**, with the whole-program step it shares with
  cross-file monomorphization.
