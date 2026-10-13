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
| `E00xx` | lexer and parser | lexer implemented (wave 1) |
| `E01xx` | types | wave 4 |
| `E02xx` | pattern matching | wave 4 |
| `E03xx` | coroutines | wave 10 |
| `E04xx` | inheritance and traits | wave 4 |
| `E05xx` | entities and systems | wave 11 |
| `E06xx` | networking | wave 16 |
| `E07xx` | effects, determinism, capabilities | wave 5 |
| `E08xx` | unsafe and sandboxing | wave 5 |
| `E09xx` | compile-time evaluation | wave 7 |
| `E10xx` | units and dimensions | wave 4 |

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

Lexing never stops at the first problem: a malformed token is reported, flagged
with `TokenFlags::kHasError`, and replaced with the best recovery value, so one
broken literal does not hide the structure of the rest of the file.

## Planned catalog

The entries below are specified but not implemented. They are listed here so
that codes are allocated once and tests can be written against the catalog
rather than against whatever the implementation happens to do.

### Errors

| Code | Catches |
|---|---|
| `E0101` | mismatched types |
| `E0102` | wrong number or type of arguments |
| `E0103` | use of an uninitialized variable |
| `E0104` | possible null dereference of a `T?` |
| `E0105` | assignment to a `let` binding |
| `E0106` | reference stored in a field (references are second class) |
| `E0107` | value escapes its `region` |
| `E0108` | use after move of a `Box<T>` |
| `E0201` | non-exhaustive `match` |
| `E0301` | `await` outside an `async` function |
| `E0302` | `async` function called without `await` or `spawn` |
| `E0401` | `override` without a virtual method, or a missing `override` |
| `E0402` | unimplemented trait method |
| `E0501` | two systems write the same component without an ordering |
| `E0502` | cycle in system ordering |
| `E0601` | server-only function called from a client context |
| `E0602` | RPC parameter type is not serializable |
| `E0701` | effect not declared in the signature |
| `E0702` | effect not permitted here (deterministic or hot context) |
| `E0703` | capability not granted to this module |
| `E0801` | `unsafe` in a module that forbids it |
| `E0901` | `comptime` assertion failed |
| `E0902` | `comptime` evaluation needs an effect |
| `E1001` | dimension mismatch |
| `E1002` | unknown unit |

### Warnings

| Code | Catches |
|---|---|
| `W0101` | unused variable, parameter or import |
| `W0102` | unreachable code, condition always true or false |
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
| `W0114` | implicit narrowing that loses data |
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
package manifest, and from wave 5 by `@allow(code, reason = "...")` in source,
where the reason is mandatory.
