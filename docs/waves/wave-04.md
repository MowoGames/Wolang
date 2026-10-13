# Wave 4 — the type system, units, generics and traits

**Status:** complete. 374 unit tests, all passing in Debug, sanitizers and
Release.

## Scope

Give the compiler types. The exit criterion was that `examples/package` type
checks end to end and that a dimension mismatch is a compile error — both of
which it now does, including across module boundaries.

The wave also carried a grammar half, finished first: Unicode identifiers,
nesting block comments, `extend`, error sets, postfix `T!`, array and slice
types, storage layout, loop labels, partial ranges and structural patterns in
conditions.

## What was built

### Grammar

| Feature | Notes |
|---|---|
| Unicode XID identifiers, NFC enforced | tables generated from Unicode 16.0 by `tools/gen/gen_unicode_tables.py` into `include/wolang/base/unicode_tables.inc`; `E0013`, `E0014` |
| Nesting block comments | `/* /* */ */`; `E0012` on an unterminated one |
| `extend T` and `extend T as Trait` | replaces `impl`, target first (D21) |
| `error NotFound, Corrupt` | one declaration binds several tags |
| Postfix `T!` | reads left to right and combines as `Pawn?!` (D13) |
| `[T]` and `[T; N]` | language constructs, not library types (D10) |
| `soa [Particle]`, `aos [Particle]` | layout belongs to the storage (D22) |
| Loop labels, `break outer` | `E0044` when no enclosing loop carries the label |
| Partial ranges `..b`, `a..`, `..` | |
| `n?` and `null` patterns | no `Some`/`None` vocabulary (D12) |
| Structural patterns in conditions | `if let Point { x } = p` |

### Types — `include/wolang/sema/type.h`, `src/sema/type.cc`

`TypeId` is a `uint32` into an interning store, so type equality is one
integer compare. Every `Type` is zeroed before interning, which lets
comparison be a `memcmp` over the whole struct, padding included.

That has a trap worth stating once: a zeroed `element` or `base` reads as
`TypeId{0}` — a real type — rather than as an invalid id. Walking a type
without first checking that its kind uses the field recurses forever. Both
are now guarded by `CarriesElement` and `CarriesBase`, and the header says
so where the field is declared. The bug surfaced as a stack overflow on
`fun Pair<A, B>(a: A, b: B) -> (A, B)`.

The store also owns:

- **Generic machinery.** `MentionsParameter`, `Unify` and `Substitute`. A
  parameter already inferred cannot be inferred differently, which is the
  error worth reporting at a call site.
- **The dimension registry.** A base dimension is interned by its
  declaration `(name, file, item)`, so `Length` declared once is one
  dimension everywhere it is imported, and a product prints with its declared
  names in any file. A file-local index made two unrelated dimensions compare
  equal and printed `?` for an imported one.

### Dimensions — `include/wolang/sema/dimension.h`

A product of base dimensions with integer exponents. Multiplying adds
exponents, dividing subtracts, and the result is dimensionless exactly when
everything cancels — that is the whole of dimensional analysis, and it is why
`10m / 2s` has a type nobody declared.

A derived dimension is declared rather than written inline (D22a):

```wolang
dimension Length
dimension Time
dimension Velocity = Length / Time
dimension Area = Length^2
```

A dimension name **is** a type: the type of quantities carrying it. So
`speed: Velocity` is how a velocity is declared, and type syntax stays free of
operators.

### Name resolution — `include/wolang/sema/resolution.h`

`ResolveFile` binds every name a file declares in five passes: type
declarations first so a declaration can refer to one that appears later,
then module-level names, then derived dimensions, then units, then the
duplicate check.

`ResolveSignatures` is separate, and the separation is the design:

- A field's type may name an imported type, so signatures need the file's
  whole scope.
- A file's *declarations* must not need another file's signatures, or two
  files of one module would each wait on the other.

An `extend` block declares no name of its own — it is named after the type it
extends — and contributes its methods to that type's member table, which is
what makes `extend` additive across files.

### Type checking — `include/wolang/sema/type_check.h`

Bidirectional: an expectation travels down, an inferred type travels up. A
literal without a suffix takes the type its context expects and settles on its
default when there is none.

What it checks:

- assignment, arity, operators, indexing, literal range;
- **member access**, including `?.` reaching through an optional and bare
  field names inside a method, where `health` means `self.health`;
- **constructors**, filling a type's fields in declaration order; no
  arguments asks for the default value;
- **generic calls**, inferring parameters from the arguments — no turbofish
  and no explicit type argument (D18);
- **trait conformance**, from the member table rather than from the block
  that names the trait, so a method may come from anywhere;
- **exhaustive `match`**, naming what is missing;
- **nullability**, with `if let`, `?.`, `??`, an `x?` pattern, and an
  explicit `p != null` in either order.

Narrowing shadows the name in the current scope, so it applies to the branch
and ends with it. No flow graph is needed for that.

### Queries

Three new ones, and the split between them is the wave's load-bearing
decision:

| Query | Depends on | Why separate |
|---|---|---|
| `DeclaredNames` | the file's own tree | no imports, which is what breaks the cycle two files of one module would otherwise form |
| `Resolution` | `DeclaredNames` of the file, its siblings and its imports | the scope: names only |
| `MemberTables` | `Resolution` of the file | signatures and members, which need the whole scope |
| `TypeCheck` | `MemberTables` of the file, its siblings and its imports | a field access reaches any type the file can see |

Member tables are merged at the `TypeCheck` level rather than inside
`Resolution`, because merging them earlier would make two module siblings
depend on each other.

Member tables live in the query result, not in the type store. A store is
shared mutable state outside the compared value: a field's type changing
there would leave `Resolution` comparing equal, back-dating, and every
reader stale. `Database.MemberTablesSurviveABodyEdit` and
`EditingAFieldTypeChangesTheMemberTable` pin both halves of that.

### Diagnostics

`E0101`–`E0112` for types and members, `E0113`–`E0116` for resolution and
dimension declarations, `E0201` for a non-exhaustive `match`, `E0402` for an
unimplemented trait method, `E1001`–`E1002` for dimensions and units.

Two rules keep the output honest:

- An error type propagates silently, so one unknown name produces one
  message rather than one per use.
- A file the parser could not finish is not type checked at all, because
  every type inferred around an error node is noise.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| `fun`, `need`, `offer` replace `fn`, `import`, `pub` (D20a) | whole words, and `need`/`offer` is a matched pair that says what a module does; the old spellings are reserved so habits get a message |
| A derived dimension is declared, not written inline (D22a) | the arithmetic is confined to one declaration, and a dimension name is then a type |
| A dimension id is global, interned by declaration | a file-local index made two unrelated dimensions compare equal |
| Member tables belong to the query result | the type store is outside the compared value, so a field type changing there would leave readers stale |
| Signatures are a separate query from resolution | a field's type may be imported, but a file's declarations must not wait on another file's signatures |
| A bare name matching an enum variant is that variant | D14's short form; treating it as a binding made every enum `match` look exhaustive |

## Bugs this wave found

- **A zeroed `TypeId` is a valid id.** Following `element` on a tuple reached
  type 0 and recursed forever. Guarded by kind, and documented in the header.
- **Item indices are file-local.** Patching an imported binding's signature
  with the importing file's `item_types` produced a function with no
  parameters. Every patch is now guarded by the file the binding came from —
  the same mistake was waiting in the dimension tables.
- **`extend Pawn` declared `Pawn`.** It is named after its target, so the
  name collided with the declaration itself.
- **A bare variant name was a catch-all.** `Point => 0.0` bound a local named
  `Point`, which made every enum `match` look exhaustive and shadowed the
  variant.
- **`null != p` was a type mismatch.** The null literal's type was used as
  the expectation for the other side.

## Things worth remembering

- `examples/turret.wo` is the parser and formatter corpus, and `wolang check`
  on it reports unknown names: it is one file, so its imports resolve to
  nothing, and class members in a single file still need their declarations.
  `examples/package` is the example that type checks end to end.
- Two files of one module declaring the same name is `E0057`, reported on
  both. That is what caught the engine stub declaring `Velocity` as both a
  dimension and a component.
- A guarded `match` arm covers nothing. Counting it would make the check
  unsound in exactly the case a guard exists for.

## Deferred to later waves

- **Monomorphization.** Inference and substitution are done; emitting one
  specialization per instantiation belongs with the IR in wave 8.
- Trait bounds (`where T: Actor`) are parsed and the parameters are in scope,
  but a bound is not yet checked against the argument.
- `E0401`: `override` without a virtual method.
- Effects and capabilities (`E07xx`), which is wave 5.
- `self` is the enclosing type, not a reference to it; second-class
  references arrive with the memory rules.

## Next

Wave 5: effects, capabilities and the rest of the checking layer. See
`STATUS.md`.
