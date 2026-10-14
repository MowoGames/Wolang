# Wave 5 — effects, capabilities and the checker

**Status:** complete. 423 unit tests, all passing in Debug, sanitizers and
Release.

## Scope

Finish the checking layer: the type debts wave 4 left, second-class
references, the first lints with suppression, and the headline feature —
effects and capabilities as **one** mechanism.

The exit criterion was a diagnostic catalog v1 where every lint has a
"fires" test and a "does not fire" test. The second one matters more: it is
what catches a lint that is right in the example and wrong everywhere else.

## What was built

### The type debts

| Feature | Notes |
|---|---|
| `override` against the base chain | all three halves: `override` with nothing to override, hiding a base method without `override`, overriding something not `virtual` (`E0401`) |
| Trait bounds at call sites | `E0403`, checked once a parameter is inferred |
| `TypeRelations` | a named type's base class and the traits it implements, merged across files like member tables |

Conformance is **declared**, not inferred from the methods present:
`extend T as Trait` is the declaration, and `Satisfies` walks the base chain
and the trait list. A bound naming a base class accepts a derived type, and a
trait implemented on the base accepts it too.

A type parameter carries its bounds (`TypeStore::Parameter` takes them), so a
call site checks a bound without reaching back into the declaring file. That
is the same trick as a dimension product travelling with its binding.

### References are second class

A parameter or a local may hold a reference; a field may not (`E0120`). That
is what gives memory safety without a borrow checker — a reference can never
outlive the frame it was made in. Region escape (`E0121`) waits for wave 9,
where `arena` actually allocates.

### Lints and suppression

| Code | Catches |
|---|---|
| `W0101` | an unused local, parameter or `need` |
| `W0102` | unreachable code |
| `E0070` | `@allow` missing a code or a reason |

An unused local is reported where it goes out of scope, which is also where
the last chance to use it was. A name starting with `_` opts out. A
declaration with no body — a trait method, an `extern` — has nowhere to use
its parameters, so it is exempt; an invalid span is what carries that.

`W0102` fires once per block rather than once per statement after the jump.

An unused `need` is checked by the type checker rather than by import
resolution, because only type checking knows what a file actually named. A
name in type position counts, which is why the resolver got a usage sink. A
bare `need a.b` brings in a whole module, so there is no one name to blame.

`@allow(W0101, reason = "...")` applies to one declaration and everything
inside it, takes several codes, and **requires a reason**: a suppression
without one is how a codebase forgets why it stopped checking something. A
missing reason is `E0070`, and the codes are still honoured while the author
fixes the attribute — otherwise the lint they named would pile on top of the
complaint about the attribute.

### Effects — `include/wolang/sema/effect.h`

An `EffectRow` is a sorted set of `EffectId`, so two equal rows compare equal
and a cached row can be back-dated. The built-in effects (`alloc`, `io`,
`rand`, `time`, `net`, `gpu`, `unsafe`, `panic`) occupy the first ids in
enumerator order, so testing for one is an index comparison; a declared
`effect` is interned by its declaration, so a project's own `effect io` is a
different effect from the built-in.

An empty row is not "unknown": it is the positive claim that a function does
nothing observable, which is what `comptime` requires. "Not stated" is the
absence of an entry, which is a different thing — and keeping those two
apart is most of the design.

### The query split that makes inference possible

Inference inside a module is a fixpoint. The obvious shape — one query per
declaration, asking its callees — is **wrong**: mutual recursion makes it a
query cycle, and the engine's cycle detection would give an answer that
depends on which declaration was asked about first. Order-dependent results
in an incremental compiler are poison.

So the work is split three ways:

| Query | Depends on | Does |
|---|---|---|
| `TypeCheck(file)` | `MemberTables` | *collects* each body's direct row and its callees. Reports no effect diagnostics. |
| `ModuleEffects(module)` | `TypeCheck` of its files | runs the fixpoint over the whole module at once |
| `EffectDiagnostics(file)` | `ModuleEffects(own module)` | compares what each declaration does against what it states |

A cycle of mutually recursive functions is now a loop *inside* one
computation. `ModuleScopeTest.EffectsPropagateThroughMutualRecursion` pins
it.

Reporting is separate from collecting for exactly one reason: if `TypeCheck`
reported effect errors it would have to depend on `ModuleEffects`, which
depends on `TypeCheck`.

### The rules

- A declaration that **states** a row must state everything it does
  (`E0701`).
- A declaration that states **nothing** is not contradicted — but its
  inferred row still reaches its callers.
- The exception is `offer`: inference stops at the module boundary, so a name
  other modules can call has to say what it does (`E0704`). Without this
  rule, cross-module checking would need `ModuleEffects(A)` →
  `ModuleEffects(B)` → `ModuleEffects(A)`.
- An `unsafe { }` block **is** the `unsafe` effect, so a caller that forbids
  it does not have to look inside.

### Contexts and capabilities

A context outranks a signature — declaring an effect does not make it
allowed:

| Context | Forbids |
|---|---|
| `comptime` | everything but `panic` |
| `@Deterministic` | `rand`, `time`, `net` |
| `@Hot` | `alloc` |

That is `E0702`, and it is one mechanism rather than three unrelated
attribute checks — which is the whole reason effects are a language feature
and not a lint.

`[build] capabilities = "io, alloc"` grants a package a set and nothing more
(`E0703`). Omitting the key leaves it unrestricted; `capabilities = ""`
sandboxes it completely, which is the setting a mod from a player's machine
gets. The check looks at the declared row as well as the inferred one, so
`fun Roll() uses rand {}` is caught even with an empty body today.
`unsafe = "deny"` is the same idea narrowed to one effect (`E0801`).

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| No implicit numeric conversions, not even widening (D15a) | a conversion that is always spelled is one that can always be found; arbitrary-precision literals remove the tiresome case. Retires the planned `W0114`. |
| An uninterpolated string literal is `text`, not `Text` | static data in the binary, not an allocation. Interpolation builds a string at runtime, which is what makes *that* owned. |
| Trait conformance is declared, not structural | `extend T as Trait` is the declaration; a type that happens to have the right methods has not promised anything |
| Effect inference is one query per module | a per-declaration query makes mutual recursion a query cycle, and the answer would depend on evaluation order |
| An `offer` function with effects must state them | the only alternative is a cycle between modules' effect queries |
| An empty effect row is a claim, not ignorance | `comptime` needs to say "nothing observable"; "not stated" is the absence of an entry |

## Bugs this wave found

- **`uses unsafe` did not parse.** `unsafe` is a keyword, and the effect list
  was parsing type references. The list now accepts the spelling.
- **`fun Name() -> text => "actor"` was a type error.** A string literal was
  always `Text`, so the idiomatic borrowed return did not type check.
- **An unknown `extend` target was reported twice** — once by the member
  pass and once by the trait check, which resolved it again.
- **Two test fixtures asserted on all diagnostics**, so the first lint broke
  32 tests that only meant "this compiles". `Codes()` is now errors only,
  with `Warnings()` beside it.

## Things worth remembering

- A `need` item has no `name_span` — it declares no name. The unused-import
  warning pointed nowhere until it used `item.span` instead, and the missing
  location is what made five correct warnings look like false positives.
- `Checker::Lookup` marks a local used, so it cannot be `const`. The
  compiler found that immediately; the alternative would have been a
  parallel usage table that drifts.
- A capability name is matched against the built-ins first, then against the
  module's declared effects — so `capabilities = "Telemetry"` works for a
  project's own effect.

## Deferred to later waves

- `E0121` (value escapes its `arena`) and `E0122` (use after move) wait for
  wave 9, where the memory rules exist.
- `E0123` (use of an uninitialized variable) needs the same flow analysis.
- `W0103` (a local shadowing a field) and the rest of the `W01xx` catalog.
- Quick fixes: `Fix` and `FixEdit` are plumbed through the renderer, but no
  lint offers one yet. `W0101` is the obvious first — "prefix it with `_`".
- The formatter puts every attribute and every block on its own line, while
  the spec writes a short `@Edit var x: float` inline. Still a policy choice
  to settle alongside the line-breaking work wave 3 deferred.

## Next

Wave 6: the typed SSA IR, lowering and the verifier. See `STATUS.md`.
