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
| `E0120`–`E0124` | memory, references and conversions | `E0120` (wave 5), `E0123`, `E0124` (waves 8–9) |
| `E0201` | pattern matching | implemented (wave 4) |
| `E03xx` | coroutines | wave 10 |
| `E0401`–`E0403` | inheritance, traits, bounds | implemented (waves 4–5) |
| `E05xx` | entities and systems | wave 11 |
| `E06xx` | networking | wave 16 |
| `E0701`–`E0704` | effects, determinism, capabilities | implemented (wave 5) |
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
| `E0011` | reserved word used as an identifier | `let transactional = 1` |
| `E0012` | unterminated block comment | `/*` with no `*/` |
| `E0013` | code point cannot appear in an identifier | `let 🚀 = 1` |
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
| `E0024` | expected an expression | `let a = *` |
| `E0025` | expected a pattern | `for in items {}` |
| `E0026` | unmatched delimiter | a block without its `}` |
| `E0028` | expected a block | `fun f() ???` |
| `E0029` | duplicate modifier | `pub offer fun f() {}` |
| `E0034` | expected a match arm | `match v { => 1 }` |
| `E0035` | expected a `state` declaration | a `fn` inside a `machine` |
| `E0036` | expected a transition | a stray name inside a `state` |
| `E0037` | expected a unit definition | `unit m` with no dimension |
| `E0040` | expected a line break between statements | `let a = 1 let b = 2` |
| `E0041` | nesting too deep | hundreds of nested parentheses |
| `E0042` | unknown type parameter in a `where` clause | `fun f<T>() where U: A {}` |
| `E0043` | unexpected token after an expression | `"{a b}"` |
| `E0044` | no enclosing loop carries that label | `break nowhere` |
| `E0045` | expected an array length | `[int; ]` |

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
| `E0053` | invalid manifest value | `name = p`, `unsafe = "maybe"`, an unknown severity |
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

A name starting with `_` is never reported, which is how an unused parameter
is marked deliberate. A declaration with no body — a trait method, an
`extern` — has nowhere to use its parameters, so it is exempt. `W0102` is
reported once per block rather than once per statement after the jump.

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
| `E0801` | `unsafe` where the package forbids it | `unsafe { }` under `unsafe = "deny"` |

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
| `E0101` | mismatched types | `let x: bool = 1` |
| `E0102` | wrong number of arguments | calling a two-parameter function with one |
| `E0103` | no value with that name is in scope | `let x = nothing_here` |
| `E0104` | a value that may be absent was used directly | `p.field` where `p: Pawn?` |
| `E0105` | assignment to a `let` binding | `let a = 1` then `a = 2` |
| `E0106` | the callee is not callable | calling an integer |
| `E0107` | the value cannot be indexed | `n[0]` where `n: int` |
| `E0108` | an operator got the wrong kind of operand | `-s` where `s: str` |
| `E0109` | the literal does not fit its type | `let x: u8 = 300` |
| `E0110` | no type with that name is in scope | `fun f(a: Nothing)` |
| `E0111` | the name is not a type | a function used in type position |
| `E0112` | the type has no such member | `v.z` where `Vec3` has `x` and `y` |
| `E0117` | a value type contains itself, so it has no size | `struct S { var next: S }` |
| `E0118` | a declaration is defined in terms of itself | `type A = A`, `class A : A`, `dimension D = D` |
| `E0119` | a type parameter is declared twice | `fun f<T, T>()` |
| `E0113` | the name is declared twice in one file | |
| `E0114` | no dimension with that name | `unit m: Nothing` |
| `E0115` | a unit conversion needs a literal with a unit | `unit cm = 5` |
| `E0116` | a dimension is a product of dimension names | `dimension D = 2 + 2` |
| `E1001` | dimensions do not match | `10m + 2s` |
| `E1002` | no unit with that name is in scope | `10parsecs` |
| `E0201` | `match` does not cover every value | a `bool` match with only `true` |
| `E0402` | a trait method is not implemented | `extend P as T` missing one method |
| `E0403` | a type argument does not satisfy a bound | `Hurt(rock)` where `T: Damageable` |
| `E0401` | `override` disagrees with the base method | marked `override` with nothing to override |
| `E0120` | a field holds a reference | `var r: &int` |
| `E0123` | a variable is used before anything is written to it | `var x: int` then `return x` |
| `E0124` | the conversion is not between numbers | `s as int` where `s: str` |
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

A type whose members are not known — declared in a file this one cannot see —
yields an error type without an `E0112`, because the unresolved import is
already reported and one cause should produce one message.

`E0117` is about storage, not about recursion: a value type stores its
members inline, so `struct S { var next: S }` describes something of
infinite size, and so do `S?`, `[S; 4]` and `(int, S)` inside it. One
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
| `E0121` | value escapes its `region` |
| `E0122` | use after move of a `Box<T>` |
| `E0301` | `await` outside an `async` function |
| `E0302` | `async` function called without `await` or `spawn` |
| `E0501` | two systems write the same component without an ordering |
| `E0502` | cycle in system ordering |
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
| `W0107` | reference held across an `await` |
| `W0108` | `loop` in an `async` function with a path that never awaits |
| `W0109` | signal subscription that outlives the subscriber |
| `W0110` | replicated field written on a client |
| `W0111` | `self` used after `Destroy()` |
| `W0112` | recursion with no base case |
| `W0113` | weak reference dereferenced without a liveness check |
| `W0115` | unreachable state in a `machine` |

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
| `H0101` | `var` never reassigned, use `let` |
| `H0102` | did you mean a similarly spelled name |
| `H0103` | null check that could be `?.` |
| `H0104` | redundant explicit type |
| `H0105` | unit conversion that could be written with a unit literal |

## Rendering

Diagnostics are rendered with the source line, markers under the relevant
spans, notes and machine-applicable fixes:

```
warning[W0107]: actor reference `t` held across `await`
 --> combat/turret.wo:3:3
  |
2 |   let t = target
  |       - `t` captured here
3 |   await Wait(0.5s)
  |   ^^^^^^^^^^^^^^^^ `t` may be destroyed while waiting
  |
  = note: re-check liveness after the await
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
