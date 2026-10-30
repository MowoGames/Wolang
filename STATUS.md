# Where we stopped

> Read this first when work resumes. What it says is true as of the date
> below; if it disagrees with the code, the code is right and this file
> needs fixing.

**Date:** 2026-10-30
**Waves are closed through the 11th**; after that the work was "a complete
language" without waves, and everything below belongs to it (D119–D229).

## In short

The language builds, computes, prints, opens a window, listens on a
socket, speaks HTTPS, reads five configuration formats, keeps state on
disk and embeds in someone else's program. Three tiers — the interpreter,
the JIT and LLVM AOT — give one answer, and a differential fuzzer checks
that they do.

| | |
|---|---|
| Tiers | interpreter (the reference), arm64/x64 dev tier for `run`, LLVM for `build` |
| Platforms | macOS arm64/x86-64, Linux x86-64/arm64, Windows x86-64 (clang-cl) |
| Cross targets | `--target`: Windows COFF, Linux ELF, WebAssembly, RISC-V |
| std | 34 modules (see [docs/stdlib.md](docs/stdlib.md)) |
| Packages | `http` (server and client), `db` (storage), `gfx` (window/GPU/sound), `tls`, `game`, `core` |
| Tests | 754 unit tests + 213 library tests, a three-tier fuzzer, a check of every example in the docs |
| Server | 53,900 requests/s on one thread, 58,900 on eight |

## Done since 27.10: the audit (D221–D229)

An audit of the compiler, the runtime, the sandbox and the libraries that
read other people's bytes, and the fixes for what it found.

**Safety.** A bodiless `uses unsafe` primitive (`Free`, `ArrayNew`,
`ViewFromRaw`) no longer absorbs `unsafe`, which closed a way for a
sandboxed mod to reach raw memory (D221); naming a function as a value
costs its effects (D222). A reference cannot leave through a field, an
index, a call or a local array turned into a slice (`E0121`), and an
object made in an `arena` cannot leave its block (`E0142`, D228). A
`deinit` that keeps `self` traps in every tier (`E0145` for the plain
case), and a move out of a `match` is a move (D229).

**Servers and readers.** The HTTP server parses only the bytes that
arrived (it used to read the previous client's), refuses a `Content-Length`
that would overflow and any `Transfer-Encoding`, and closes slow and idle
connections (D223). JSON, YAML, TOML and regex bound their depth, and
`InflateLimited`/`GunzipLimited` bound their output (D227).

**Tiers.** Labelled `break`/`continue` on range-`for` and `while` work;
folding leaves 64-bit overflow and `MIN / -1` to trap; `u64` interpolates
above 2^63; float `%` is `fmod` (D224). Sign-changing 64-bit casts trap in
the compiled tiers, trap text is identical everywhere, floats print as the
shortest round-trip text (D226). Every depth is bounded where it is made:
long expression chains are `E0041`, runaway generics `E0140`, recursive
enums `E0117`, a literal too wide for `int` is `E0109`, and the
interpreter stops before the native stack runs out (D225).

**CI.** AOT is built and fuzzed on every system, `lib/db` and `lib/tls`
are tested, there is a UBSan job, and `fuzz --aot` fails without a back
end instead of passing.

## Done since 15.10 (D191–D200)

**Memory.** `class` is reference counted (D193): the count lives in the
word before the object, and whoever lets go last calls `deinit` and
frees it. D206 closed the leaks: an object releases its fields (`$drop`
per class), a write to a field, global or local releases the old value,
`T?` is counted, passing to a function is a borrow, temporaries are
released. The live-object counter `wolang_objects_live` is for leak
tests. Only cycles leak.

**LSP.** The first quick fix, and `wolang fix`, which applies them
(D191).

**Windows for real (D201–D205).** Before this the port only compiled:
the CLI did not build, the x86-64 tier spoke System V instead of the
Windows convention and returned 0 from `Add(2, 3)`, a trap crashed in SEH
unwinding, `std` called POSIX directly, time zones did not know
`Europe/Kyiv`, and `gfx` looked for a `.so`. Now everything is green on
Windows: 45 unit-test suites, `std`/`http`/`game`/`core`/`gfx`, the
fuzzer, the examples, the docs, formatting, and a C host through a DLL.
Shared problems found along the way: CI had never run (the repository was
on GitLab and CI was in `.github`), `fmt --check lib` worked nowhere,
`wolang.h` did not compile as C, and `std.time.Now()` ticked in
milliseconds on Linux (a coarse clock).

**Codegen.** A constant power-of-two divisor is a shift (D192). Nine
x86-64 bugs closed (D194–D200): a trampoline with stack arguments, REX
for byte operations, scratch 0 = the accumulator, a virtual call, a
narrow result, temporaries over operands, a swap slot for a slice. Found
by running x86-64 under Rosetta — 94 tests failed before.

## Done in the previous series (D148–D190)

**Platforms.** The Windows port: Winsock, `WSAPoll`, `VirtualAlloc`,
`CreateProcess`, `LoadLibrary`, files in binary mode, threads and the
file system through the standard library (D158, D159, D184, D189).
Sockets moved into the runtime — `std.net` and `http` are now the same
everywhere. Cross-compilation with `--target` to every LLVM target
(D160). Linux arm64 and Windows added to CI.

**Window, graphics, sound.** Our own platform layer (`platform/`): Cocoa,
Win32, X11; pixels, a GL context, a sound queue. The `lib/gfx` package,
and `examples/window` at 150 frames a second (D155).

**Standard library.** A shared document type `std.doc`, and on it JSON,
TOML, YAML 1.2, INI and CSV (D162, D165). Regular expressions with their
own engine and a budget against exponential blow-up (D166). Cryptography:
SHA-256/512, HMAC, system entropy, UUID v4/v7, base32, constant-time
comparison (D170). Compression: our own deflate, gzip and CRC-32 —
compatible with the system `gzip` both ways (D177). Time zones and ISO
time parsing from the system database (D176). Processes with pipes, a
working directory and an environment (D153, D179). The rest is in
[docs/stdlib.md](docs/stdlib.md).

**Storage.** `lib/db` — a log with checksums and a sorted in-memory
index; survives a torn write, checked by hand (D181).

**Language.** `ptrnull` (D156), indexing through a reference (D167),
parenthesised types as generic arguments (D169), `[]T!` as "a slice that
may fail" (D178), arguments beyond the registers on the stack — in both
assemblers (D154).

**Shipping.** The `install.sh` and `install.ps1` installers with checksum
verification (D185), an `.msi` through WiX (D188), a `.vsix` without npm
(D175), the amalgamation — the whole compiler as one file for embedding
(D186), the logo and the mascot (D174).

**Compiler bugs found and fixed.** Twenty-odd; the most expensive: a `u8`
was written as eight bytes in the interpreter (found by ASan), arrays
inside structs were lost in memory, a nested `rows[1][2]` added the
indices, `List<List<T>>` swallowed the comma between parameters, `<` on
strings compared pointers, a value from `-> T!` was not wrapped, and a
method on a type from a transitive import silently gave an error type
(D168, D171, D172, D173, D182, D190).

## What works

```sh
# Builds with clang; details and what each system needs are in
# docs/building.md (D184).
cmake --preset debug && cmake --build --preset debug -j
ctest --preset debug

W=./build/debug/tools/wolang/wolang
$W modules examples/package      # the graph of packages and modules
$W check examples/package        # check a package, with the cache
$W fmt examples/turret.wo        # formatting
$W ast examples/turret.wo        # the syntax tree
$W ir examples/package           # IR; verifier complaints on stderr, exit 70
$W run examples/hello.wo         # run, with the differential check
$W test lib/core                 # run the `@Test` functions, in two tiers
$W asm benchmarks/gameplay.wo    # what compiled, what did not, and why
$W bench benchmarks/gameplay.wo  # run and time the `@Bench` functions
$W run --interpret <path>        # do not use the compiled tier
$W build <path> --target wasm32-unknown-unknown   # code for another machine
$W fuzz --count 200              # the three tiers on generated programs
$W lsp                           # what editors talk to
benchmarks/run.sh                # comparison with C++ (needs the release preset)

python3 tools/package/check_docs.py        # the examples in the docs
python3 tools/package/check_includes.py    # every file says what it needs
python3 tools/package/make_vsix.py         # the VS Code extension
python3 tools/package/make_amalgamation.py # the compiler as one file
```

| Layer | State |
|---|---|
| CMake skeleton, presets, test harness | done |
| `Arena`, `StringInterner`/`Symbol`, `SourceManager`, diagnostics | done |
| Lexer + 11 diagnostics `E0001`–`E0011` | done |
| AST + parser + 19 diagnostics `E0020`–`E0043`, recovery | done |
| AST printer (s-expressions) | done |
| **Query engine** (red-green, back-dating, cycles, `Seed`) | done |
| **`ItemTree`** — editing a body does not change signatures | done |
| **`Database`** — `Tokens/ParsedFile/ItemTree/ModuleScope/Imports/Diagnostics` | done |
| **`wolang.toml` manifest** + 5 diagnostics `E0050`–`E0054` | done |
| **Modules and packages** + 6 diagnostics `E0055`–`E0060` | done |
| **Formatter** (`fmt`, `--check`, `--write`) | done |
| **On-disk cache** (`.wolang-cache/items.wdb`) | done |
| CLI `check / fmt / modules / ast / tokens / version` | done |
| **Unicode XID + NFC**, nested block comments | done |
| **Wave 4 grammar**: `extend`, `error`, `T!`, `[]T`/`[N]T`, `soa`/`aos`, loop labels, open ranges | done |
| **Types** — interning, `TypeId` = `uint32`, equality = id comparison | done |
| **Dimensions and units** — `dimension`, `unit`, derived dimensions, `E1001`–`E1002` | done |
| **Name resolution** — the `DeclaredNames` / `Resolution` queries | done |
| **Signatures and members** — the `MemberTables` query | done |
| **Type checking** — bidirectional inference, the `TypeCheck` query | done |
| **Generics** — inferring parameters from arguments (unification) | done |
| **Abilities** — conformance through `extend T as Ability`, `E0402` | done |
| **Exhaustive `match`** — `E0201` listing what is not covered | done |
| **Nullability** — `if const`, `?.`, `??`, `x?`, `p != null` | done |
| **Inheritance** — `override` against the base chain, `E0401` | done |
| **Ability bounds** — checked at the call site, `E0403` | done |
| **Second-class references** — a field cannot hold a reference, `E0120` | done |
| **Lints** — `W0101` (unused), `W0102` (unreachable) | done |
| **`@allow(code, reason)`** — suppression with a required reason, `E0070` | done |
| **Effects** — fixpoint inference per module, `E0701`, `E0704` | done |
| **Contexts** — `comptime`, `@Deterministic`, `@Hot`, `E0702` | done |
| **Capabilities** — a grant per package, `E0703`, `E0801` | done |
| **Typed SSA IR** — block parameters, one id space, no renumbering | done |
| **Lowering** from the checked tree; units are erased, effects stay | done |
| **IR verifier** — dominance, edge arity and types, reachability | done |
| **Passes** — constant folding, DCE, slot promotion, flow simplification | done |
| **Interpreter** — the reference semantics, traps with a kind and a place | done |
| **Differential execution** — IR before and after the passes on every run | done |
| **`comptime`** — evaluated in the lowering query, `E0901` | done |
| **Intrinsics and interpolation** — `Print`, `IntToStr`, … | done |
| **`lib/core` in Wolang** — `cmp.wo` with `capabilities = ""` | done |
| CLI `run / test / ir` | done |
| **arm64 assembler** — encodings checked against the system assembler | done |
| **Type layout** — sizes, alignments, field offsets | done |
| **Linear scan register allocator** — callee-saved only | done |
| **arm64 codegen** — scalars in registers, aggregates in the frame | done |
| **Traps in generated code** — the interpreter's kind, place and text | done |
| **Unwinding and symbolication** — the frame chain, a backtrace on a trap | done |
| **Monomorphisation** — one specialisation per instantiation (wave 4 debt) | done |
| **Inlining** of small calls (wave 6 debt) | done |
| **Block merging** and **slot promotion with stores on several paths** (wave 6 debt) | done |
| **Effect rows in the IR are filled** by the per-module inference | done |
| CLI `asm / bench` | done |
| **C ABI FFI** — `extern`, `E0802`/`E0803`, calls from both tiers | done |
| **Raw pointers** — `*T` reads and writes, `&x`, a dereference carries `unsafe` | done |
| **`defer`** — runs on every exit from the scope | done |
| **`E0121`/`E0122`/`E0123`** — an escaping reference, use after move, an uninitialised variable | done |
| **Linking a package's files** into one compiled module | done |
| **`std.io` in Wolang** through `write`; `wolang run` runs compiled code | done |
| **`text` in the compiled tier** — a pointer and a length, literals | done |
| Owning types (`Owned`/`Shared`), `arena` as an allocator, a collector | deferred |
| **The `async` effect** — `task`/`wait`, `E0301`–`E0305` | done |
| **Task transformation** — a frame, a state machine, nested subframes | done |
| **`spawn` and the scheduler** — kind+frame, one dispatcher per module | done |
| **`std.tasks`** — `Yield`, `Run`, `Join`, `Done`, `Pending` | done |
| **Cross-file calls in the interpreter** — linked by declaration | done |
| **The target platform seam** under the emitter and the thunk | done |
| **Methods, classes on the heap, function values** | done |
| **`for`/`foreach`, writes to a field and an element, slices** | done |
| **Signals** — a signal's type is its listener's type; `Connect`/`Emit` | done |
| **`derived`** — computed once and kept until an input changes | done |
| **`machine`** — the transition table becomes methods | done |
| **Strings that compile** — one model for every tier | done |
| **`arena { }` as an allocator** — freed on any exit | done |
| **`wolang-http` and `examples/server`** — 30k requests/s at the time | done |
| Destructors, reference counting for `class` (D193, D206) | done; only cycles leak |
| Concurrent connections (kqueue/epoll/`WSAPoll`) | done |
| **x86-64** — assembler, emitter, thunk, the `x64` preset | done |
| Other architectures (riscv64, wasm) | through LLVM `--target` |
| **ECS** (`component`, `system`, a compiler-scheduled frame), **AOT through LLVM** | done |
| **Window, GPU and sound** — our own backend per system, the `gfx` package | done |
| **`lib/db` storage** — a log with checksums, survives a torn write | done |
| **Five formats** — JSON, TOML, YAML 1.2, INI, CSV on one tree | done |
| **Regular expressions**, **cryptography**, **compression**, **time zones** | done |
| **Windows** — Winsock, WSAPoll, VirtualAlloc, CreateProcess, clang-cl | done |
| **Cross-compilation** with `--target` to every LLVM target, WebAssembly included | done |
| **Installers** — `install.sh`, `install.ps1`, `.msi`, `.vsix`, the amalgamation | done |
| **Documentation** — the tour, the cookbook, the std reference; examples checked in CI | done |

Wave reports: [`wave-01`](docs/waves/wave-01.md),
[`wave-02`](docs/waves/wave-02.md), [`wave-03`](docs/waves/wave-03.md),
[`wave-04`](docs/waves/wave-04.md), [`wave-05`](docs/waves/wave-05.md), [`wave-06`](docs/waves/wave-06.md), [`wave-07`](docs/waves/wave-07.md),
[`wave-08`](docs/waves/wave-08.md),
[`wave-09`](docs/waves/wave-09.md).
Packages and modules: [`docs/packages.md`](docs/packages.md).

## Decisions taken (do not revisit without a reason)

The full list, with every reason, is [docs/decisions.md](docs/decisions.md).
These are the ones that shape everything else.

1. **A separate project**, not an engine module. The engine is one of the
   hosts; its adapter comes in wave 15.
2. **Hybrid memory:** values with mutable value semantics, second-class
   references, `Owned`/`Shared`, `arena`, and `class` + GC as an
   **optional** component.
3. **Effects and capabilities** — one mechanism instead of five checks.
4. **Four signature features in the core:** units of measure, AoS/SoA +
   SPMD `foreach`, `machine`/`state` automata, reactive `derived`.
5. **Contextual keywords** (`state`, `unit`, `machine`, `derived`,
   `effect`, `dimension`, `enter`, `exit`, `always`, `with`, `without`,
   `weak`) are lexed as identifiers.
6. **A unit suffix on a number is just a symbol**; Sema resolves it.
7. **The standard library is in Wolang** over a thin C++ platform layer;
   intrinsics lower straight into the IR. `core` is possible from wave 7.
8. **The compiler will not be self-hosted.** The compiler is C++20.
9. **Chromium code style everywhere**, no exceptions and no RTTI; commits
   are Conventional Commits, one line, no body and no co-authored trailer.
10. **arm64 before x64** for the dev codegen.
11. **The IR interpreter is the reference semantics.**
12. **`docs/` and `STATUS.md` are in English** (D220, replacing D7).
13. **Control constructs are expressions.**
14. **`as` is the cast operator.**
15. **No turbofish:** `f<T>(x)` is resolved by looking ahead at tokens.
16. **A dot in an expression is always member access.**
17. **An operator at the start of a line starts a new statement**; the
    exceptions are `.` and `?.`.
18. **A module is a directory**, not a file. The module name is the
    package name + the path below the source root.
19. **Query dependencies are discovered, not declared** — everything a
    computation reads it must request through the engine.
20. **A cached value cannot contain a source position.** Positions
    belong to the revision they were taken in; diagnostics resolve them
    against the current tree.
21. **`ItemTree` contains no positions and no handles** — otherwise
    editing a body would change it and defeat the point of incrementality.
22. **Package discovery is not a query.** The engine tracks only what it
    is told about; the file system is not part of that.
23. **The manifest is declarative TOML.** External tools (CI, a registry,
    dependency-update bots) must read a package's name and version without
    a Wolang interpreter. A `build.wo` build script may appear **next to**
    the manifest in wave 7+, like `build.rs` next to `Cargo.toml`.
24. **The cache is purely an optimisation.** A stale, corrupt, truncated
    or foreign cache is discarded, not used.
25. **A file outside a package does not resolve imports** and does not
    report `E0055`–`E0058`: checking a single file is a normal case.
26. **`fun` / `need` / `offer`** instead of `fn` / `import` / `pub`. `need`
    and `offer` are a pair: a module **needs** names from outside and
    **offers** its own. The old spellings are reserved so that habit gives
    a message rather than a puzzling parse error.
27. **`extend T` / `extend T as Ability`** instead of `impl`; the target
    comes first. `extend` declares nothing — it is named after the type it
    extends and adds methods to its member table.
28. **A derived dimension is declared, not written in place:**
    `dimension Velocity = Length / Time`. The arithmetic is locked in the
    declaration, and general type syntax stays free of operators. **The
    dimension's name is the type** of a quantity with that dimension, so
    `speed: Velocity`.
29. **A dimension's identifier is global**, interned by declaration. A
    per-file index made two unrelated dimensions equal.
30. **Member tables live in a query result**, not in the type store. The
    store is shared mutable state outside the compared value: changing a
    field's type there would leave `Resolution` equal to itself,
    back-dating would fire, and every reader would stay stale.
31. **Signatures are a separate query from name resolution.** A field's
    type may refer to an imported type, but a file's declarations must not
    depend on another file's signatures — otherwise two files of one
    module would wait on each other.
32. **A bare name that matches an enum variant is the variant**, not a new
    binding. Otherwise every `match` on an enum would look exhaustive.
33. **No implicit numeric conversions**, not even widening: `s32` to `s64`
    is written with `as`. Arbitrary-precision literals take away the
    tedium. Cancels the planned lint `W0114`.
34. **A string literal without interpolation is `text`**, not `Text`: it
    is static data, not an allocation. With interpolation the string is
    built at run time — and that is what makes it owning.
35. **Conformance to an ability is declared, not inferred structurally.**
    `extend T as Ability` is a declaration; a type that happens to have the
    same methods promised nothing.
36. **Effect inference is one query per module.** A query per declaration
    makes mutual recursion a query cycle, and the answer would depend on
    evaluation order. Order dependence is unacceptable in an incremental
    compiler.
37. **An `offer`ed function with effects must declare them.** The only
    alternative is a cycle between the effect queries of different modules.
38. **An empty effect row is a statement, not ignorance.** `comptime`
    must be able to say "nothing observable"; "not specified" is the
    absence of a row, a different state.
39. **Effect rows reach the IR**, rather than being erased with the units.
    "This call does nothing observable" is exactly what allows removing it
    as dead. This answers wave 5's open question.
40. **Block parameters instead of phi nodes.** The same information, but
    where the verifier already checks edge arity and types.
41. **A pass never renumbers.** An id is an index; compaction would mean
    rewriting every operand in the function. A dead instruction becomes
    `undef`, a removed block becomes empty.
42. **An empty block is a removed block.** Otherwise removal cannot be
    expressed without renumbering.
43. **The verifier runs on every input, twice** — after lowering and
    after the passes. The IR is a contract; a pass bug and a lowering bug
    must be told apart.
44. **Dominance by iterative sets**, not Lengauer-Tarjan. Slower, and far
    simpler to be sure of — the right trade for the one component that is
    not allowed to be wrong.
45. **Lowering runs only on a file without errors.** Otherwise every
    value would carry a "maybe" into every back end.
46. **A layer's vocabulary lives in a nested namespace** (`wolang::ir`).
    This is not tidiness: a second `wolang::Module` is an ODR violation,
    and the symptom was memory corruption in another test.
47. **A trap is a defined outcome, not a crash**: it has a kind and a
    place. Otherwise the later tiers have nothing to be compared with.
48. **Overflow traps rather than wrapping.** Silent wrapping turns an
    arithmetic error into a wrong answer somewhere else.
49. **So arithmetic is not pure** (decision D15c): removing an unused
    `a + b` would turn a program that traps into one that does not.
    Removing it needs a proof that it cannot trap — that is range analysis.
50. **`Text` is accepted where `text` is wanted** (decision D15b) — an
    implicit borrow, safe because references are second class. Without it,
    printing an interpolated string would not type-check.
51. **The step limit is a property of a run, not of a program.**
    `comptime` and running a program want different budgets.
52. **`comptime` is evaluated in the lowering query**, not during type
    checking: during checking it would be a query cycle.
53. **Differential execution on every run.** A miscompile is too serious
    to look for in a nightly build — that is how DCE removing a division
    by zero was found.
54. **`as` is a numeric conversion and nothing else, and narrowing
    traps** (decision D41). D33 removed implicit widening, so `as` is the
    only numeric conversion; reinterpreting bits is a job for `unsafe`.
    Converting something non-numeric is `E0124`.
55. **A generic function is not lowered at all** (decision D42). It is a
    recipe, not a function: lowering it would put `T` into the IR, and
    every back end would have to answer for it. One specialisation per
    instantiation, named by its arguments: `Identity<s32>`.
56. **A trap in generated code is a check before the operation, not a
    signal handler** (decision D43). This answers wave 8's open question.
    A check is portable and predictable; a handler is free on the hot path
    but tied to the platform and impossible on WASM. Range analysis will
    remove the check — the same analysis that removing dead arithmetic
    needs.
57. **The allocator hands out only callee-saved registers** (decision
    D44). Nine integer registers instead of twenty-odd — but nothing has to
    be saved around a call: whatever the allocator placed, the callee must
    restore.
58. **An aggregate lives in the frame and is passed by pointer**
    (decision D45). One rule for every aggregate, wherever it came from;
    scattering fields across registers is the optimising tier's job.
59. **A trap is reported by a non-local exit** (decision D46). The path
    without a trap pays nothing. The trap state must live outside automatic
    storage: an object changed between `setjmp` and `longjmp` is
    indeterminate afterwards — and the optimiser folded the trap kind into
    the zero it was initialised with. Only the release build found this.
60. **The trap vocabulary belongs to the IR, not the interpreter**
    (decision D47). Division by zero is a property of the operation; both
    tiers must name it the same way, or the differential check compares two
    vocabularies.
61. **A trap's text says only what both tiers know** (decision D48). The
    interpreter could print the index itself; generated code cannot put a
    run-time value into a string built at compile time.
62. **Every compiled function builds a frame record, even a leaf**
    (decision D49). Two instructions in the prologue — and walking the
    stack costs two loads per frame and needs no tables at all.
63. **A tier compiles what it can and says what it could not** (decision
    D50). The interpreter is the reference semantics and always at hand, so
    refusing is cheap, and a guess would be the one thing nobody could
    debug.
64. ~~**`wolang run` stays on the interpreter until FFI**~~ (decision D51)
    — **lifted in wave 9**: FFI arrived, `std.io.Print` is written in
    Wolang, and `wolang run` runs compiled code when it can; `--interpret`
    forces the interpreter.
65. **A call outside the language is the `unsafe` effect.** One sentence
    and no new machinery: an `offer` must declare it, `@Deterministic`
    cannot reach C, and a package with `unsafe = "deny"` cannot call C at
    all. FFI adds exactly one diagnostic of its own — `E0802`.
66. **A declared `uses unsafe` absorbs the obligation rather than
    re-exporting it** (decision D58). `unsafe` is not a capability like
    `io` or `net`: those say what the code needs from the outside world,
    and the caller must know. `unsafe` says that someone checked. Without
    this, a safe wrapper over an unsafe primitive cannot exist:
    `std.io.Print` would make every printing program unsafe.
67. **The C boundary accepts numbers, `bool` and pointers** (`E0802`).
    Everything that crosses it must mean the same thing on both sides, and
    a type whose layout this compiler alone decides — a slice, a string, an
    optional — does not.
68. **Both tiers reach C the same way.** Compiled code calls the address
    from the loader; the interpreter calls through a thunk generated by the
    same assembler. Otherwise FFI would be the one feature nothing checks.
69. **A program runs once** (decision D59). Wave 7 ran it twice and
    compared — worth it while a program could only fill a sink owned by the
    driver. Once a program can `write`, two runs write twice. The
    comparison moved to `wolang test`: a test is the one place where the
    program is known to finish.
70. **A declaration is linked by its item, not by its name.** The IR
    knows only unqualified names; two files may each declare a private
    `Helper`, and only the item the declaration points at says which one
    this is.
71. **`defer` runs from the inside out, after the return value is
    computed.** A `defer` cannot change the answer — only what remains
    afterwards.
72. **`const`/`var` instead of `let`/`var`** (decision D52): two visually
    different words instead of a word with a modifier. Mutable locals are
    the most common, and `var x` must not cost more than the immutable form.
73. **`task fun` and `wait` instead of `async`/`await`** (decision D53).
    Shorter, and `wait` says what the machine does rather than naming a
    protocol. The alternative — making suspension an effect in the `uses`
    row and dropping `wait` entirely — is more interesting and may return
    once the effect system has a concurrency wave behind it; for now the
    suspension point is visible at the call.
74. **`Owned<T>`/`Shared<T>` and `arena`** (decision D54): they say in
    English what the relationship is; `arena` is the word gamedev already
    uses for this allocator.
75. **`s32`, not `i32`** (decision D55): both halves of the width table
    are integers, and only one is signed.
76. **`text`/`Text` instead of `str`/`String`** (decision D56): lower case
    is a view, PascalCase is a type that owns its bytes. That is the same
    rule every declared type follows.
77. **`[]T` and `[N]T`** (decision D57): the brackets give the shape
    before the element, and the type reads left to right.

---

## Not done yet

An honest list. Everything else that used to be in this section is done
— generics across files, enum construction, `comptime` with arguments,
`for`/`foreach`, `T!`, destructors, the arena, interpolation in the
compiled tier, threads, AOT and the WASM target.

**In the language**

- ~~Associated types on `ability`~~ (D212): `type Item`, `C.Walker.Item`,
  `std.iter.{Iterator, Iterable}`; `extend<T> Box<T>` works.
- **`Owned<T>`/`Shared<T>` in full**: the type exists, but "has this been
  moved" is not threaded through lowering everywhere.
- **Reference counting** (D193, D206, D207, D209): a cycle without `weak`
  leaks; a `text` inside a `std.doc` value is a view of bytes nobody owns.

**In optimisation**

- **Strength reduction for any constant divisor** — a power of two is
  already a shift (D192); other divisors wait for a "high half of the
  product" opcode.
- **Vectorisation** and **alias analysis** — without the second the first
  gives almost nothing, and `soa` will not show more than a few percent.
- **Critical edge splitting** — the one case where slot promotion gives
  up.
- Spill slots are not reused; every temporary aggregate takes its own
  area of the frame.

**In the libraries**

- HTTP/2 and HTTP/3; a cookie jar and multipart in the client.
- Full date parsing beyond ISO (locales, `strftime` patterns).
- The X11 clipboard — returns only what the program itself put there.
- Windows arm64 — builds only under x86-64 emulation.

**In the tools**

- **An IR reader**: the printer is stable, but a dump cannot be read back
  yet.
- Coverage and sanitizers for Wolang programs.
- Dependencies by exact revision only — no ranges and no conflict
  resolution.

## The road to 1.0

1.0 is a language in which you can write an actor in MowoEngine with
`Tick`, components and hot reload, without leaks, and ship it in a
release build. The language and the engine host are one line of work, not
two tracks: until the host exists, there is no 1.0. The order follows the
dependencies.

**1. Memory without leaks**
- ~~A write to a field releases the old value~~, ~~an object releases its
  fields~~, ~~`T?` is counted~~, ~~passing to a function does not leak~~ —
  D206.
- ~~Classes inside structs, enums, `T?` and `T!`~~ (D209); classes no
  longer move, and `deinit` runs when the last reference goes.
- `std.doc`: strings in a document are owning `Text` instead of `text`.
- ~~`weak` references~~ (D207), ~~an atomic counter~~.
- `Owned`/`Shared` in full: "has this been moved" threaded everywhere.
- Cycles: weak references plus documentation; a cycle collector only if
  the benchmarks from item 9 show it cannot be done without.

**2. A language that does not break after 1.0**
- ~~Associated types on `ability`~~ (D212); `for` over `C: Iterable`.
- ~~The formatter and the specification agree on attributes~~ (an
  attribute is its own line in both); ~~the keywords~~ checked against the
  specification.

**3. Every platform, every commit**
- CI that actually runs: the repository is now on GitHub, where
  `.github/workflows/ci.yml` runs on every push; it has to be green on
  every system in the matrix.
- In the matrix: Windows x64 (done locally, D201–D205), Linux x64 and
  arm64, macOS arm64; macOS x86-64 is not in it yet, and Windows arm64
  comes later (it needs the arm64 emitter with the Windows convention).
- ~~AOT on Windows~~ (D208): `LLVM-C.dll` 22.1.8, linked with `clang-cl`.
  LLVM 23 miscompiles `x != 6 % x` in IPSCCP — pinned to 22.
- ~~TLS on Windows~~ (D211): Schannel on Windows, OpenSSL through `dlopen`
  on the other systems; HTTPS and the AOT build of `http` work everywhere.
- A bug fixed in one emitter is looked for in the other (D200) — a check,
  not a rule on paper.

**4. The boundary with the host**
- ~~Opaque handles through the C ABI~~ (D213): `extern class` is a host
  object, the host keeps the count (`wolang_vm_register_class`), and a
  `weak var` asks `alive`; `kWolangObject` is a Wolang object held by the
  host, with method calls, field reads and a `release` that runs `deinit`.
- Lists and enums with data across the boundary — not yet: a wrapper class
  covers this for 1.0.
- ~~Threads~~ (D214): a vm accepts calls from any thread, one at a time;
  for parallelism, one vm per worker.

**5. ~~Hot reload with state~~** (D215)
- ~~`wolang_vm_reload` keeps module-level `var`s and live objects~~:
  carried over by name when the shape of the type matches.
- ~~Function handles are stable by name~~.
- ~~A change to a class's layout~~: fields carry over by name, the rest is
  a line in `wolang_vm_notes`.
- Globals now belong to each vm (D214); before, they were shared by the
  process.

**6. ~~The debugger~~** (D216)
- ~~DAP~~: `wolang dap` (launch) and `wolang_vm_debug_listen` (attach to
  the engine); breakpoints, steps, the stack, locals and object fields.
  Debugging runs on the interpreter over unoptimised IR — the reference
  tier the fuzzer checks the others against; AOT is still debugged with
  lldb.

**7. ~~The sandbox~~** (D217)
- ~~`extern` only in trusted packages~~: in the sandbox it is `E0705`.
- ~~Capabilities checked all the way~~: the capability is written on the
  primitive's `extern` (`uses io/net/time/rand`) and is not absorbed by a
  wrapper; ~220 signatures in `lib` fixed by following the compiler's
  hints.
- `wolang_vm_load_mod(vm, path, caps)`: the host sets the grant, the mod's
  manifest can only narrow it; `unsafe` and `[link]` are forbidden.

**8. The MowoEngine host** (wave 15; done in the engine's repository once
items 1–7 are ready — until then the engine is not touched)
- ~~The bindings generator~~ (D218): `wolang bind --reflection` over the
  engine's reflection JSON → a Wolang package and C++ glue; 165 classes,
  734 functions, 256 properties; the glue compiles against the engine's
  headers. What remains on the engine side is four functions (a
  property's offset, retain/release/alive) and calling the generator in
  the build.
- A bridge to `MObject` and actors: handles, roots, weak references,
  method calls, reading and writing `MPROPERTY`.
- Bindings generated from the engine's reflection (`MCLASS`/`MFUNCTION`/
  `MPROPERTY` → Wolang declarations), like the Visual Scripting node
  catalogue.
- Wolang `component`/`system` mapped onto the engine's ECS.
- The editor: a "Wolang (Preview)" project template, creating classes
  from the Content Browser, errors in the Message Log, reload in PIE,
  debugging over DAP, LSP for VS Code and Rider.
- `mbt package` compiles scripts through AOT; there is no JIT in a
  release build.

**9. The proof**
- ~~The actor benchmark~~ (D219): 1000 actors with `Tick` per frame — AOT
  1.7× of C++, JIT 11×, the interpreter ~1400× (only under the debugger).
  `benchmarks/actors`, target `wolang_actor_bench`. The fast path
  `wolang_call` and `wolang build --library` for the engine.
- A comparison with Visual Scripting and a vertical slice (a whole level
  in Wolang, no leaks under ASan/LSan, reloading during PIE) — in the
  engine, following the integration plan.

After 1.0: SPMD `foreach`, vectorisation and alias analysis, full WASM,
HTTP/2–3, an IR reader, dependency version ranges, Windows arm64.
