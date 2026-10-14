# Wave 9 — out of the language, and back

**Status:** complete for the FFI, the memory rules and `defer`. Owning types
and a collector are deferred with reasons; see the end. 571 unit tests, all
passing in Debug, sanitizers and Release.

## Scope

Memory and the C ABI. What landed is the boundary — calling out of the
language and back, raw pointers, and the three rules that make references
and moves safe — plus the renaming of the language's surface that the
boundary made unavoidable to decide.

```
$ wolang run examples/hello.wo
hello, world

$ wolang asm <a package that prints>
// 3 compiled, 6 left to the interpreter, 548 bytes
@main: 108 bytes
@Print: 260 bytes
@write: is `extern`, called at the address the loader gave
```

That is the wave in one listing: `main` calls `std.io.Print`, which is
written in Wolang, which calls `write` through the C ABI — and all of it is
machine code.

## What was built

### The C ABI

An `extern` declaration is implemented outside the language. The syntax was
already there; what was missing was everything else.

**Calling out of the language is the `unsafe` effect.** That one sentence is
the whole rule, and it needed no new machinery: a declaration that calls out
must state `uses unsafe` when it is `offer`ed, a `@Deterministic` context
cannot reach C, and a package with `unsafe = "deny"` cannot call out at all
(`E0801`). The FFI adds exactly one diagnostic of its own, `E0802`: the C
boundary takes numbers, `bool` and pointers, because everything crossing it
has to mean the same on both sides and a type whose layout this compiler
decides unilaterally — a slice, a string, an optional — does not.

**A declared `uses unsafe` absorbs the obligation** rather than re-exporting
it (D58). This was forced by the first real library function: without it
`std.io.Print` would make every program that prints an unsafe one, and the
capability model would have nothing left to say. `unsafe` is not a
capability like `io` or `net` — it is an audit, and the declaration that
states it is the one taking responsibility.

**Both tiers reach C the same way.** Compiled code calls the address the
loader gave; the interpreter calls through a thunk generated with the same
assembler — one thunk for every signature, because the only thing a
signature changes is how many registers hold something. Without that, every
program using the FFI would have fallen out of the differential check.

### Raw pointers

`*T` was a type the checker understood and nothing else did: a dereference
lowered to its own operand, silently. Now `*p` is a load and `*p = v` a
store, in both tiers, and `&x` is the address of a local.

Reading through a raw pointer is `unsafe` and a reference is not, which is
the whole difference between the two. With `malloc` and `free` reachable
through the FFI, that makes the heap usable before there is a type for it.

### `defer`

Parsed since wave 4, never run. A `defer` is the one statement whose
position in the source is not where it runs, and lowering is where that
stops being true: it becomes ordinary code at every exit from the scope that
deferred it — the end of the block, a `return`, a `break`, a `continue` —
innermost first.

The returned value is computed before the deferred statements run, so a
`defer` cannot change the answer, only what is left behind. With the FFI
that is memory management today: allocate, defer the release, and the
release happens on every path out.

### The three memory rules

| Code | Catches | How |
|---|---|---|
| `E0121` | a reference that would outlive what it points into | scope depth per local; a `region`'s value and a `return` are the two ways out |
| `E0122` | a use after a move | per-local state, with writes intersecting and moves unioning at a join |
| `E0123` | a variable used before anything is written to it | definite assignment |

All three are one walk over the syntax, which is where they have to be: the
IR is only built for a file that produced no errors, so a check that reports
one cannot run after lowering.

`E0123` is the one that mattered most, because it removes the only place the
two execution tiers disagreed: the interpreter traps on reading a slot
nothing wrote, and compiled code reads the zero its prologue left. Making
the program impossible is better than making the tiers agree about it.

`E0122` needed something only the checker knows: whether a use moves or
borrows. That turned out to be the question "what did the context want" — a
`Text` where a `Text` is expected moves, and a `Text` where a `text` is
expected is the implicit borrow of D15b — so the checker now records which
expressions it accepted as borrows, and the flow pass reads that rather than
guessing from a type.

### The dev tier compiles a program, not a file

Lowering is per file, so a call into another file is a declaration with no
body. One file at a time, that left every cross-module call to the
interpreter — in a package, most of them. The code generator now takes every
file at once and links a declaration to its definition **by the declaration
it came from**, a file index and an item index, rather than by name: two
files may each define a private `Helper`, and only the item it points at
says which one this is.

That is what made the listing at the top of this report possible.

### `text` in compiled code, and the end of D51

A `text` is a pointer and a length, so it is an aggregate like any other,
and a literal is a view of bytes that live with the module. With that and
the FFI, `std.io` is Wolang:

```wolang
extern "C" fun write(fd: s32, buffer: *u8, count: u64) -> s64

offer fun Write(message: text) uses io, unsafe {
  const _written = write(1, message.data, message.length)
}

offer fun Print(message: text) uses io {
  Write(message)
  Write("\n")
}
```

`.data` and `.length` are built into `text` rather than declared, because
the platform floor needs the bytes and how many there are and nothing else
about a string.

D51 is settled: `wolang run` uses the compiled tier when it can, and
`--interpret` forces the other one.

**A program now runs once.** Wave 7 ran it twice and compared, which was
worth it when the only thing a program could do was fill a sink the driver
owned. Now that a program can call `write`, running it twice writes twice: a
side effect is not something to repeat for the compiler's benefit. The
comparison moved to `wolang test`, where every `@Test` runs in both tiers —
tests are the one place a program is known to terminate and known to be
worth running twice.

### The renaming

Decided mid-wave, and the right moment: all of it is cheaper now than after
wave 10 writes a concurrency library in the old words.

| Was | Is | Why, in one line |
|---|---|---|
| `let` / `var` | `const` / `var` | two distinct words; mutable locals are most of them and must not cost more to write |
| `async` / `await` | `task` / `wait` | shorter, and `wait` says what the machine does |
| `region` | `arena` | the word the gameplay world already uses for exactly this |
| `i32` | `s32` | both halves of the table are integers; only one is signed |
| `str` / `String` | `text` / `Text` | lowercase for the view, PascalCase for the type that owns its bytes |
| `[T]` / `[T; N]` | `[]T` / `[N]T` | the shape before the element, so a type reads left to right |
| `Box` / `Rc` | `Owned` / `Shared` | they say in English what the relationship is |

The words that were replaced are **reserved**, so writing one gets a
diagnostic instead of a confusing parse failure. See D52–D57.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| Calling out of the language is the `unsafe` effect | every rule already in place then applies, and there is no second rule to keep in step |
| A declared `uses unsafe` absorbs it (D58) | otherwise no safe abstraction over an unsafe primitive is possible |
| The C boundary takes numbers, `bool` and pointers (`E0802`) | everything crossing has to mean the same on both sides |
| Both tiers reach C through the same ABI | a program using the FFI stays inside the differential check |
| A program runs once (D59) | a side effect is not something to repeat for the compiler's benefit |
| A declaration is linked by its item, not by its name | two files may each define a private `Helper` |
| `defer` runs innermost first, after the returned value is computed | a `defer` may not change the answer |
| D52–D57, the renaming | see `docs/decisions.md` |

## Bugs this wave found

- **A dereference took the context's type.** `CheckUnary` passed the
  expectation down to its operand, which is right for `-x` and wrong for
  `*p`: `return *p` checked the *pointer* against what the pointee has to
  be, reported a mismatch on the operand, and then a second error on the
  dereference of the error type it became. Two diagnostics for a correct
  program.
- **A `defer` never ran.** Parsed since wave 4 and dropped by lowering,
  which is worse than not parsing it: the program read as if it released
  something.
- **A dereference lowered to its own operand**, so `*p` quietly produced the
  pointer.
- **Scope depth cannot be counted in locals.** The first `defer`
  implementation keyed a deferred statement to `locals_.size()`, and a block
  that declares nothing does not change it — so an inner `defer` looked like
  an outer one and never ran. It needs a block counter of its own.
- **The keyword rename exposed a stale corpus.** `examples/turret.wo` was
  returning a `Text` from a function whose arms were a mix of owned and
  borrowed strings; nothing had noticed, because the file is the
  unresolved-imports corpus and nobody had read past the names.

## Things worth remembering

- The `unsafe` effect is an *obligation*, not a capability. Everything else
  in the row says what the code needs from the outside world and must reach
  the caller; `unsafe` says someone checked, and stops at the declaration
  that says so.
- A foreign call is the one place where "both tiers do the same thing" costs
  real work — a thunk, generated at runtime, for the interpreter. It was
  worth it: the alternative is that the FFI is the one feature nothing
  checks.
- Linking by name is a guess. The IR knows only unqualified names, and the
  only reason cross-file calls resolve correctly is that lowering records
  the declaration each one came from.
- A deferred statement is analysed by the flow pass where it is *written*,
  not where it runs. That is a known gap, written down in
  `docs/diagnostics.md` rather than left for someone to discover.

## Deferred to later waves

- **`Owned<T>` and `Shared<T>`.** Both need destructors: a type that frees
  something when it goes out of scope. `defer` is the machinery that makes
  destructors possible, which is why it came first — the remaining work is
  the type-directed part, deciding *what* to run and threading "has this
  been moved" into lowering.
- **`arena` as a real allocator.** The keyword parses and the escape rule
  (`E0121`) is enforced, but nothing allocates from it yet: that needs an
  ambient allocator, which is the `alloc` effect with a value attached.
- **The optional collector.** The wave's exit criterion — a program with no
  `class` links no collector — holds, but vacuously: there is no collector
  to not link. It becomes a real claim when `class` allocates.
- **An owned `Text` in compiled code**, and with it interpolation: building
  a string needs a heap, which is the same dependency as `Owned<T>`.
- A `*T` to an aggregate is read and written field by field; the interpreter
  declines rather than growing a layout of its own.

## Next

Wave 10: `async` — or rather `task` and `wait` — with structured
concurrency, signals, state machines, and a minimal HTTP/1.1 server as the
design's driver (D40). See `STATUS.md`.
