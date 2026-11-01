# Diagnostics

Every diagnostic has a stable code, so it can be configured, suppressed and
searched for. Codes never change meaning; retired codes are not reused.

| Prefix | Severity | Meaning |
|---|---|---|
| `E` | error | blocks compilation and hot reload |
| `W` | warning | a likely bug |
| `P` | perf | a likely performance problem |
| `H` | hint | style or simplification |

## Code ranges

| Range | Owner | Status |
|---|---|---|
| `E0001`–`E0019` | lexer | implemented (wave 1) |
| `E0020`–`E0049` | parser | implemented (wave 2) |
| `E0050`–`E0069` | manifests, modules, packages | implemented (wave 3) |
| `E0070` | diagnostic suppression | implemented (wave 5) |
| `E0101`–`E0119` | types and name resolution | implemented (wave 4) |
| `E0117`–`E0119` | sizes, cyclic declarations, parameters | implemented (wave 5) |
| `E0120`–`E0126` | memory, references, conversions, receivers, generics | `E0120` (wave 5), `E0123`–`E0124` (waves 8–9), `E0125` (wave 10), `E0126` (wave 11) |
| `E0127`–`E0129` | systems and the access graph | all wave 11 |
| `E0130` | a module-level declaration with no type written | wave 12 |
| `E0137` | `weak` fields and module-level `weak var`s | D207, D253 |
| `E0143`, `E0144` | module-qualified names | D246 |
| `E0145` | `self` stored from its own `deinit` | D229 |
| `E0147`, `E0148` | what crosses a thread | D232 |
| `E0150`, `E0151`, `E0152` | an offered function that does not state its errors, an error outside a stated set, an error with data in a bare `T!` | D235 |
| `E0153`, `E0154` | a bodiless `uses unsafe` that should be `unsafe fun`, an `unsafe fun` that overrides or implements a safe method | D236 |
| `E0155` | a `@Hot` function that allocates, directly or through what it calls | D237 |
| `E0156`, `E0157` | `Self` outside a type, an ability that takes `Self` used as a value | D239 |
| `E0159` | an `after` whose delay is not a number of seconds | D243, D260 |
| `E0160` | an `@Operator` ability that is not one operator-shaped method | D239 |
| `E0161`, `E0162` | a borrowed function value in a place that keeps it, an escaping closure that takes a reference, pointer, slice or borrowed function | D240 |
| `E0164` | a script class that derives from an `extern class` | D256 |
| `E0165` | an element written through a slice that is not `[]mut T` | D251 |

| `E0149` | an owning field moved out of the value that holds it | D233 |
| `E0201` | pattern matching | implemented (wave 4) |
| `E0301`–`E0305` | suspension and tasks | implemented (wave 10) |
| `E0401`–`E0403` | inheritance, abilities, bounds | implemented (waves 4–5) |
| `E05xx` | entities and systems | wave 11 |
| `E06xx` | networking | wave 16 |
| `E0701`–`E0705` | effects, determinism, capabilities | implemented (wave 5, `E0705` in D217) |
| `E0801` | forbidden `unsafe` | implemented (wave 5) |
| `E09xx` | compile-time evaluation | wave 7 |
| `E10xx` | units and dimensions | implemented (wave 4) |
| `W0101`–`W0102` | unused names, unreachable code | implemented (wave 5) |
| `W01xx` | the rest of the warnings | waves 6+ |

## Implemented: lexical errors

| Code | Message | Example |
|---|---|---|
| `E0001` | unterminated string literal | `"abc` |
| `E0002` | unknown escape sequence | `"a\qb"` |
| `E0003` | invalid character literal | `''`, `'ab'`, `'a` |
| `E0004` | malformed number literal | `0x`, `0b1012`, `0o18` |
| `E0005` | unexpected character | `$` |
| `E0006` | invalid UTF-8 encoding | a stray `0xFF` byte |
| `E0007` | unterminated or empty interpolation | `"{}"`, `"{health"` |
| `E0008` | integer literal does not fit in 64 bits | `18446744073709551616` |
| `E0009` | invalid Unicode escape | `"\u41"`, `"\u{110000}"`, `"\u{D800}"` |
| `E0010` | invalid numeric suffix | `1f128`, `1.5u8`, `0xFFm` |
| `E0011` | reserved word used as an identifier | `const transactional = 1` |
| `E0012` | unterminated block comment | `/*` with no `*/` |
| `E0013` | code point cannot appear in an identifier | `const 🚀 = 1` |
| `E0014` | identifier is not in Unicode NFC form | `и` plus a combining breve |

Lexing never stops at the first problem: a malformed token is reported, flagged
with `TokenFlags::kHasError`, and replaced with the best recovery value, so one
broken literal does not hide the structure of the rest of the file. A run of
characters the lexer cannot start a token with is reported once, not once per
character.

## Implemented: syntax errors

| Code | Message | Example |
|---|---|---|
| `E0020` | expected a declaration | `42` at file scope |
| `E0021` | expected an identifier | `fun 1() {}` |
| `E0022` | expected a specific token | `class C` without `{` |
| `E0023` | expected a type | `fun f(a: ) {}` |
| `E0024` | expected an expression | `const a = *` |
| `E0025` | expected a pattern | `for in items {}` |
| `E0026` | unmatched delimiter | a block without its `}` |
| `E0028` | expected a block | `fun f() ???` |
| `E0029` | duplicate modifier | `pub offer fun f() {}` |
| `E0034` | expected a match arm | `match v { => 1 }` |
| `E0035` | expected a `state` declaration | a `fn` inside a `machine` |
| `E0036` | expected a transition | a stray name inside a `state` |
| `E0037` | expected a unit definition | `unit m` with no dimension |
| `E0040` | expected a line break between statements | `const a = 1 const b = 2` |
| `E0041` | nesting too deep | hundreds of nested parentheses, or a chain of a thousand `+1` (D225) |
| `E0042` | unknown type parameter in a `where` clause | `fun f<T>() where U: A {}` |
| `E0043` | unexpected token after an expression | `"{a b}"` |
| `E0044` | no enclosing loop carries that label | `break nowhere` |
| `E0045` | expected an array length | `[]int` |
| `E0046` | `keep` anywhere but on the function type of a parameter (D240) | `class C { var f: keep fun() }` |

`E0026` carries two labels: the primary one where the closing delimiter was
expected, and a secondary one on the delimiter that was left open.

Parsing never stops either. An unparsable construct becomes an error node, the
parser resynchronizes on the next declaration, statement or closing brace, and
the declarations around the broken one still reach the tree. The parser also
suppresses its own message when the offending token was already reported by
the lexer, or when the attributes and modifiers in front of a declaration
already produced one, so each broken spot yields one diagnostic.

## Implemented: package and module errors

| Code | Message | Example |
|---|---|---|
| `E0050` | manifest syntax error | a line without `=`, an unclosed table header |
| `E0051` | unknown manifest key or table | `author = "me"` under `[package]` |
| `E0052` | missing manifest key | no `[package]`, no `name`, a dependency without a path |
| `E0053` | invalid manifest value, a version requirement that is malformed or that the copy found does not meet, or a bare-string dependency (D242) | `name = p`, `version = "^x"`, `core = "1.2"` |
| `E0054` | duplicate manifest key | `name` given twice in one table |
| `E0055` | unresolved module | `need game.nowhere` |
| `E0056` | a needed name is not declared | `need game.math.{Vec4}` when there is no `Vec4` |
| `E0057` | name declared more than once in a module | two files declaring `fun Fire` |
| `E0058` | a needed name is not offered | needing a declaration without `offer` |
| `E0059` | cyclic package dependencies | two packages depending on each other |
| `E0060` | package cannot be read | no manifest, no source directory, unreadable file |

A manifest problem never stops the load: the manifest keeps whatever was
understood, so one bad line does not hide the rest of a project. A file
checked on its own, outside any package, resolves no imports and reports none
of `E0055`–`E0058` — compiling a single file is a legitimate thing to do.

Per-code severity comes from the `[lints]` table of the package that owns the
file, including `"off"` to silence a code entirely:

```toml
[lints]
W0105 = "off"
P0101 = "error"
```

## Implemented: warnings

| Code | Message | Example |
|---|---|---|
| `W0101` | a name is never used | an unused local, parameter or `need` |
| `W0102` | this code is never reached | a statement after `return` |
| `W0109` | a state nothing leads to | `state Stuck` with no transition to it |

A name starting with `_` is never reported, which is how an unused parameter
is marked deliberate. A declaration with no body — an ability method, an
`extern` — has nowhere to use its parameters, so it is exempt. `W0102` is
reported once per block rather than once per statement after the jump.

A machine is a table, so it is checked as one: `E0206` for a transition to
a state that is not there, and `W0109` for a state nothing leads to. Both
are mistakes somebody makes while editing a table, which is why the spec
asks for them.

An unused `need` is checked by the type checker rather than by import
resolution, because only type checking knows what a file actually named. A
bare `need a.b` brings in a whole module, so there is no one name to call
unused.

## Implemented: effects and capabilities

| Code | Message | Example |
|---|---|---|
| `E0701` | a declaration does an effect it does not state | `fun f() uses io` whose body reaches `rand` |
| `E0702` | this context forbids the effect | `@Deterministic` reaching `rand` |
| `E0703` | the package was not granted the capability | `uses net` under `capabilities = "io"` |
| `E0704` | an offered signature does not state its effects | `offer fun f()` whose body does `io` |
| `E0705` | a sandboxed package declares `extern` | `extern class wolang { fun system(...) }` in a mod |
| `E0801` | `unsafe` where the package forbids it | `unsafe { }` under `unsafe = "deny"` |
| `E0301` | `wait` outside a `task` function | `fun f() { wait g() }` |
| `E0302` | a `task` called without `wait` or `spawn` | `fun f() { g() }` where `g` is a task |
| `E0303` | `wait` is not the whole of its expression | `const x = n + wait f()` |
| `E0304` | a task waits on itself | `task fun f() { wait f() }` |
| `E0305` | `wait` on a task from another module | `wait other.Work()` |
| `E0802` | a type with no C representation crosses the `extern` boundary | `extern "C" fun f(s: text)` |
| `E0803` | an `extern` function has a body | `extern "C" fun f() { }` |

A row is inferred inside a module and stated on offered signatures. A
declaration that states nothing is not contradicted, but its inferred row
still reaches its callers — the exception is `offer`, because inference stops
at the module boundary and a caller elsewhere has nothing else to read.

`E0702` outranks a signature: declaring an effect does not make it allowed in
a context that forbids it. `E0703` is not a mistake in the code but a
permission the package was not given, which is the distinction that makes
mod sandboxing explainable.

## Suppression

```wolang
@allow(W0101, reason = "kept for the ABI")
fun Legacy(unused: int) {}
```

`@allow` applies to one declaration and everything inside it, takes several
codes, and **requires a reason** — a suppression without one is how a
codebase forgets why it stopped checking something. A missing reason is
`E0070`; the codes are still honoured while the author fixes the attribute,
so the lint they named does not reappear on top of the complaint.

Per-code severity from the manifest's `[lints]` table still applies, and
`"off"` there silences a code for a whole package.

## Implemented: type errors

| Code | Message | Example |
|---|---|---|
| `E0101` | mismatched types | `const x: bool = 1`; an `int` is `s64`, so `xs.len` or an `int` passed to an `s32` parameter needs `as s32` |
| `E0102` | wrong number of arguments | calling a two-parameter function with one |
| `E0103` | no value with that name is in scope | `const x = nothing_here` |
| `E0104` | a value that may be absent was used directly | `p.field` where `p: Pawn?` |
| `E0105` | assignment to a `const` binding | `const a = 1` then `a = 2` |
| `E0106` | the callee is not callable | calling an integer |
| `E0107` | the value cannot be indexed | `n[0]` where `n: int` |
| `E0108` | an operator got the wrong kind of operand | `-s` where `s: text` |
| `E0109` | the literal does not fit its type | `const x: u8 = 300`; with no type, from 2^63 (`int` is `s64`) |
| `E0110` | no type with that name is in scope | `fun f(a: Nothing)` |
| `E0111` | the name is not a type | a function used in type position |
| `E0112` | the type has no such member | `v.z` where `Vec3` has `x` and `y` |
| `E0117` | a value type contains itself, so it has no size | `struct S { var next: S }`, `enum E { X(e: E) }`, `struct W<T> { var w: W<W<T>>? }` |
| `E0118` | a declaration is defined in terms of itself | `type A = A`, `class A : A`, `dimension D = D` |
| `E0119` | a type parameter is declared twice | `fun f<T, T>()` |
| `E0113` | the name is declared twice in one file | |
| `E0114` | no dimension with that name | `unit m: Nothing` |
| `E0115` | a unit conversion needs a literal with a unit | `unit cm = 5` |
| `E0116` | a dimension is a product of dimension names | `dimension D = 2 + 2` |
| `E1001` | dimensions do not match | `10m + 2s` |
| `E1002` | no unit with that name is in scope | `10parsecs` |
| `E0201` | `match` does not cover every value | a `bool` match with only `true` |
| `E0402` | an ability method is not implemented | `extend P as T` missing one method |
| `E0403` | a type argument does not satisfy a bound | `Hurt(rock)` where `T: Damageable` |
| `E0401` | `override` disagrees with the base method | marked `override` with nothing to override |
| `E0120` | a field holds a reference | `var r: &int` |
| `E0137` | `weak` on something that is not a `C?` field of a class or a module-level `var` | `weak var count: int` |
| `E0138` | a `type` outside an ability with nothing after it | `type Count` |
| `E0139` | an `extern class` constructed, or given something other than methods | `Actor()` |
| `E0143` | a name two `need`ed modules both offer is used bare (D246) | `need json` and `need ini`, then `Parse(text)` |
| `E0144` | `module.Name` where the module offers no such name, or does not offer it; also in a `match` pattern (D246) | `json.Nothing(x)` |
| `E0145` | `self` stored into a field or global from its own `deinit` | `deinit { stash = self }` |
| `E0147` | a function or closure given to a `@Thread` parameter (`Spawn`, `ParallelFor`) reads or writes a module-level `var`, directly or through a call or a method, in any module, and the caller does not state `uses unsafe`; a callee that states it stops the walk (D232, D252) | `Spawn(Work)` where `Work` pushes to a global list |
| `E0148` | a closure that captures given to a `@Thread` parameter that has no environment to give it (`Spawn`; not `ParallelFor`), even under `uses unsafe` (D232) | `Spawn(\|\| { Print("{n}") })` |
| `E0159` | `after <delay>` where the delay is not a number or a quantity: the generated `Tick(dt)` counts seconds up to it (D243, D260) | `on Interact after "soon" -> Closed` |
| `E0150` | an `offer fun` whose result is a bare `T!`: a caller in another module cannot see which errors it holds, so the set is written (D235) | `offer fun F() -> int!` |
| `E0151` | an error returned, or passed on with `?`, that the stated set does not contain; or a `?` from a bare `T!` into a stated set (D235) | `fun F() -> int!(A) { return B }` |
| `E0152` | an error that carries data where the result is a bare `T!`, which has no room for it; name it in the set (D235) | `fun F() -> int! { return P(1) }` with `error P(x: int)` |
| `E0153` | a function with no body that writes `uses unsafe`: nothing vouches for it, so it is the caller's promise; write `unsafe fun` (D236) | `fun Free(items: []u8) uses unsafe` |
| `E0154` | an `unsafe fun` that overrides a method, or implements an ability method, that is not `unsafe`: a call through the base would skip the promise (D236) | `override unsafe fun F() {}` over `virtual fun F() {}` |
| `E0155` | a `@Hot` function that constructs a class, builds owned text, takes storage with `ArrayNew`/`SoaNew`, builds an error with data, or calls something that does; the note lists the calls (D237) | `@Hot fun Tick() { Grow() }` with `fun Grow() { const b = Box() }` |
| `E0158` | a name in a `uses` clause that is no effect: a typo, or `alloc`, which is not one any more (D237) | `fun F() uses alloc {}` |
| `E0156` | `Self` where there is no type for it to be: outside an ability, an `extend` and a type's own body (D239) | `fun F(x: Self)` |
| `E0157` | an ability whose method names `Self` used as the type of a value; use it as a bound instead (D239) | `const e: Eq = a`, `fun F(e: Eq)` |
| `E0161` | a function value from a parameter that is not `keep` stored, returned, or given to a `keep` parameter, field or kept local (D240) | `fun Hold(f: fun()) { saved = f }` |
| `E0162` | a closure that outlives its call taking a reference, a pointer, a slice or a borrowed function with it (D240) | `fun M(r: &int) -> fun() -> int { return \|\| r }` |
| `E0165` | an element written, or `&mut` taken of one, through a slice that is `[]T` and not `[]mut T`; a write through a handle the slice holds is not one (D251) | `fun Fill(xs: []int) { xs[0] = 1 }` |
| `E0164` | a script `class` with an `extern class` as its base: no host object stands behind it, and an upcast would hand the host a pointer it never made (D256) | `extern class A {}` then `class B : A {}` |
| `E0160` | an `@Operator` ability that is not exactly one method named `Plus`, `Minus`, `Times`, `Divide`, `Negate`, `Equals` or `Compare` with that operator's shape (D239) | `@Operator ability A { fun Frob(o: Self) -> Self }` |

| `E0149` | a value with a `deinit` taken out of a field of the struct or enum that owns it (D233) | `Take(o.r)` where `o: Outer` holds `r: Res` |

| `E0149` | a value with a `deinit` taken out of a field of the struct or enum that owns it, or moved on from inside a closure that captured it (D233) | `Take(o.r)` where `o: Outer` holds `r: Res` |
| `E0140` | a generic instantiates itself with ever bigger types | `fun F<T>(x: T) { F([x]) }` (D225) |
| `E0121` | a reference would outlive what it points into (also through fields, indices, calls and a local array as a slice, D228) | `return &local.n` |
| `E0142` | an object made inside `arena { }` is stored outside, returned or given out as the block's value (D228) | `kept = Leaf(1)` inside the block, `kept` declared outside |
| `E0122` | a value that was moved out of is used again | `const b = a` then `Take(a)` where `a: Text` |
| `E0123` | a variable is used before anything is written to it | `var x: int` then `return x` |
| `E0124` | the conversion is not between numbers | `s as int` where `s: text` |
| `E0125` | a method of a value type assigns to a field of `self` | `struct P { fun F() { x = 1 } }` |
| `E0126` | a generic type built with nothing to bind its parameter | `struct M<T> { var tag: int }` then `M(1)` |
| `E0127` | two systems use the same component, one writes it, and nothing says which runs first | two `system`s over `Query<mut Velocity>` |
| `E0128` | `@After` or `@Before` names something that is not a system | `@After(Grabity)` |
| `E0129` | the orderings between systems form a cycle | `@After(B)` on A and `@After(A)` on B |
| `E0130` | a module-level `const` or `var` with no type written | `const five = 5` at file scope |
| `E0131` | a `for` over something that cannot be walked | `for x in 5` |
| `E0132` | a function with a declared result can reach its end without returning one | `fun F() -> int { 42 }` |
| `E0133` | a type parameter appears nowhere in a signature, so no call can say what it is | `fun F<T>(n: int) -> bool` |
| `E0134` | a closure parameter whose type nothing says | `const f = \|n\| n + 1` |
| `E0135` | a closure naming a local of the function around it | `const k = 1` then `\|n\| n + k` |
| `E0070` | `@allow` is missing a code or a reason | `@allow(W0101)` |

An error type propagates silently: one unknown name produces one message, not
one per use of the value it was assigned to. Type checking is also skipped
entirely on a file the parser could not finish, because every type inferred
around an error node is noise.

`E0201` names what is missing rather than only saying the match is
incomplete — the uncovered enum variants, or `true`/`false`, or `null`/`x?`.
Only bool, optionals and enums have a value set the compiler can enumerate;
every other type needs a catch-all arm. An arm with a guard covers nothing,
because the guard may be false.

`E0303`, `E0304` and `E0305` are the shape of the transform showing through,
and each says so rather than pretending otherwise. A task is split into a
state machine whose frame holds everything that outlives a suspension, so:
a `wait` has to be the whole of its expression, because anything else in
that expression would be a value the frame has nowhere to keep; a task
cannot wait on itself, because its frame would contain itself; and a `wait`
on a task from another module needs the frame layout that module computed,
which lowering -- one file at a time -- does not have. The first will relax
when the transform spills values of its own; the last when there is a
whole-program step, the same one cross-file monomorphization needs.

Suspension is both a word and an effect. `wait` is where a function
suspends and `task` is how a signature says it may, which keeps the
suspension point visible at the call site; `async` in the effect row is what
makes it *checkable*, so a `comptime` or `@Hot` context forbids suspending
with the same machinery that forbids allocating. `task` counts as stating
the effect, so nobody writes it twice. `spawn` starts work without waiting
for it: everything the task does carries over to the spawner -- starting
work is being responsible for it -- except `async`, because the spawner does
not suspend.

Calling an `extern` function is the **`unsafe` effect**, which is how every
rule already in place applies to the FFI with nothing new to write: a
declaration that calls out has to state `uses unsafe` when it is `offer`ed,
a `@Deterministic` context cannot reach C, and a package with
`unsafe = "deny"` cannot call out at all (`E0801`). The one thing the FFI
adds is `E0802`: the C boundary takes numbers, `bool` and pointers, because
everything crossing it has to mean the same on both sides, and a type whose
layout this compiler decides unilaterally — a slice, a string, an optional —
does not.

`E0121` is the escape rule for a second-class reference (D8): a reference may
be passed inwards and never kept somewhere that outlives it. Two scopes can
be failed to outlive -- a `arena`, whose value leaves it, and the call
frame, which a `return` leaves.

`E0122` is the other half of the same walk. A type that owns a resource --
today a `Text`, later an `Owned<T>` -- cannot be in two places, so handing it
somewhere moves it and the name it was in no longer holds a value. Which use
moves and which merely borrows is the question "what did the context want":
a `Text` where a `Text` is expected moves, and a `Text` where a `text`
is expected is the implicit borrow of D15b. A write gives the name a value
again. Writes merge by intersection and moves by union: a value moved on one
branch is gone at the join, while a value written on one branch is not
written. A loop body is walked twice, because the second iteration moving
what the first one moved is the mistake worth reporting.

A deferred statement is analysed where it is written rather than where it
runs, so a `defer` that uses something moved after it is not reported. The
alternative is to analyse the deferred statement at every exit, which is
what lowering already does with it; the check will follow when `defer` has a
reason to be part of the move rules.

`E0123` is definite assignment: a variable is reported when *any* path to a
use fails to write it, which is why a loop body does not count -- it may not
run. It is also what makes the two execution tiers agree, because the
interpreter traps on reading a slot nothing wrote and compiled code reads the
zero its prologue left. A variable is initialized as a whole: writing one
field of an uninitialized struct is still a use of the struct.

`as` converts between numbers and does nothing else. It exists because D33
removed implicit widening, not as a reinterpretation of bits -- so a
narrowing conversion that would change the value traps, the same way an
overflowing add does, and converting anything that is not a number is
`E0124` rather than a cast that quietly means something.

`E0125` is the difference between a value and a handle, reported where it
would otherwise be invisible. A method of a `struct` is passed a copy of
the object, so assigning to a field of `self` would change the copy and
nothing else; a method of a `class` is passed the handle, so the same line
changes the object. Before the receiver existed at all this compiled to
nothing -- which is the kind of silence a diagnostic exists to replace.

A type whose members are not known — declared in a file this one cannot see —
yields an error type without an `E0112`, because the unresolved import is
already reported and one cause should produce one message.

`E0117` is about storage, not about recursion: a value type stores its
members inline, so `struct S { var next: S }` describes something of
infinite size, and so do `S?`, `[4]S` and `(int, S)` inside it. One
indirection is enough to have a size — a slice, a reference, or a `class`,
which is a handle to a heap object.

## Planned catalog

The entries below are specified but not implemented. They are listed here so
that codes are allocated once and tests can be written against the catalog
rather than against whatever the implementation happens to do. Codes already
in use appear in the implemented tables above, not here.

### Errors

| Code | Catches |
|---|---|
| `E0501` | reserved, unused: implemented as `E0127` |
| `E0502` | reserved, unused: ordering errors are `E0127` and `E0128` |
| `E0601` | server-only function called from a client context |
| `E0602` | RPC parameter type is not serializable |
| `E0901` | `comptime` assertion failed |
| `E0902` | `comptime` evaluation needs an effect |

### Warnings

| Code | Catches |
|---|---|
| `W0103` | local variable shadows a field |
| `W0104` | ignored result of a `@MustUse` function |
| `W0105` | floats compared with `==` |
| `W0106` | overflow or division by zero in a constant expression |
| `W0107` | reference held across an `wait` |
| `W0108` | `loop` in a `task` function with a path that never waits |
| `W0109` | (code taken by the unreachable-state warning above; needs a new code) signal subscription that outlives the subscriber |
| `W0110` | replicated field written on a client |
| `W0111` | `self` used after `Destroy()` |
| `W0112` | recursion with no base case |
| `W0113` | weak reference dereferenced without a liveness check |
| `W0115` | reserved; the unreachable-state check is implemented as `W0109` |

### Performance

| Code | Catches |
|---|---|
| `P0101` | heap allocation in a hot path |
| `P0102` | string concatenation in a hot path |
| `P0103` | lookup that should be cached in a field |
| `P0104` | loop-invariant expensive call |
| `P0105` | quadratic iteration over all entities |
| `P0106` | large struct passed by value |
| `P0107` | `foreach` could not be vectorized, with the reason |
| `P0108` | virtual call in a hot loop that cannot be devirtualized |
| `P0109` | bounds check not eliminated, with the reason |
| `P0110` | coroutine that waits every frame |

### Hints

| Code | Catches |
|---|---|
| `H0101` | `var` never reassigned, use `const` |
| `H0102` | did you mean a similarly spelled name |
| `H0103` | null check that could be `?.` |
| `H0104` | redundant explicit type |
| `H0105` | unit conversion that could be written with a unit literal |

## Rendering

Diagnostics are rendered with the source line, markers under the relevant
spans, notes and machine-applicable fixes:

```
warning[W0107]: actor reference `t` held across `wait`
 --> combat/turret.wo:3:3
  |
2 |   const t = target
  |       - `t` captured here
3 |   wait Wait(0.5s)
  |   ^^^^^^^^^^^^^^^^ `t` may be destroyed while waiting
  |
  = note: re-check liveness after the wait
```

Primary spans are marked with `^`, secondary spans with `-`, and a span that
continues past the end of its line ends with `...`. Tabs are expanded so
markers line up, and multi-byte characters count as one column. Labels pointing
into other files are reported as notes rather than being dropped.

## Configuration

Per-code severity can be overridden, including to `off`, through
`DiagnosticEngine::SetSeverityOverride`. From wave 3 this is driven by the
package manifest, and by `@allow(code, reason = "...")` in source, where the
reason is mandatory. See **Suppression** above.
