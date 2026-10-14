# Wave 2 — syntax tree and parser

**Status:** complete. 167 unit tests, all passing in Debug, sanitizers and
Release.

## Scope

Turn tokens into a syntax tree that covers every construct in
`docs/spec.md`, and make the parser recover rather than give up.

## What was built

### Syntax tree — `include/wolang/syntax/ast.h`

Flat arrays of plain-data nodes, one array per category, with typed 32-bit
handles instead of pointers. Child lists are ranges into per-category index
pools, so a file's tree is contiguous and can be serialized as-is for the
incremental cache in wave 3.

Twelve node categories: `Item`, `Expr`, `Stmt`, `TypeRef`, `Pattern`,
`Attribute`, `Param`, `GenericParam`, `MatchArm`, `State`, `Transition`,
`Ordering`.

Handles are distinct types (`AstId<Tag>`), which lets every accessor be an
overload of `Get()`:

```cpp
for (ItemId id : ast.Get(ast.root())) {
  const Item& item = ast.Get(id);
}
```

`Item` is one uniform struct for every declaration kind rather than a union.
Declarations are the least numerous nodes, and a single shape keeps the parser
and every later pass free of casts.

### Parser — `src/syntax/parser.cc`

Recursive descent for declarations and statements, Pratt for expressions, with
thirteen precedence levels. Everything in `docs/spec.md` parses: declarations,
generics with `where` clauses, effect lists, traits and impls, enums with
payload variants, components, tags, systems with ordering, signals and
handlers, bindings including `derived`, type aliases, dimensions and units,
effects, state machines, extern functions, `comptime` declarations.

Control flow is expression-shaped, as in Rust: `if`, `match`, `loop`, `while`,
`for`, `foreach`, `unsafe`, `arena` and blocks are all expressions, so
`const x = if c { a } else { b }` needs no special case.

Three decisions worth recording:

- **Line-start operators end a statement.** A `-` opening a line is unary, not
  a continuation, so two statements can never silently fold into one. Member
  access is exempt, because leading-dot method chains are normal style.
- **Generic calls are resolved by a token scan.** There is no turbofish; on `<`
  the parser looks ahead for balanced angle brackets followed by `(`. The scan
  excludes parentheses, which keeps `a < f(x) > (y)` a comparison pair at the
  cost of not accepting function types as explicit generic arguments.
- **`.` is always member access in an expression.** A dotted path like
  `World.Spawn` is parsed as field access and resolved later by name
  resolution, which is what languages without a `::` separator do.

The `>>` token is split when it closes two nested generic lists, so
`Map<K, Array<V>>` parses without a lexer hack.

String interpolation is parsed on demand: `LexSpan` lexes the bytes inside the
string token with absolute offsets, and a nested parser produces the
expression, so `"hp {hp * 100 / max}%"` becomes real syntax with diagnostics
that point inside the literal.

### Error recovery

Parsing never fails. Nineteen syntax diagnostics (`E0020`–`E0043`) each come
with resynchronization: declarations recover at the next declaration keyword at
a line start, statements at the next line or `;`, delimiters report both the
place a closer was expected and the opener that was left unclosed, and machine
bodies skip whole balanced constructs.

Two suppression rules keep one broken spot to one message: the parser stays
quiet about a token the lexer already flagged, and about a declaration whose
attributes or modifiers already produced a diagnostic. `examples/recovery.wo`
has five independent mistakes and produces exactly five diagnostics, with all
four valid declarations still in the tree.

### Tree printer — `src/syntax/ast_printer.cc`

An indented s-expression dump, used by `wolang ast` and as the golden form in
tests:

```
(class Turret
  (attr Spawnable)
  (extends
    (type Actor))
  (var range
    (of
      (type float))
    (init
      (float 15))))
```

### Driver

`wolang ast <files>` prints the tree, `--spans` annotates every node with its
position. `wolang check` now runs the parser as well as the lexer.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| `as` is the cast operator | resolves the wave 1 open question; it binds tighter than any binary operator |
| `with` and `without` are contextual keywords | needed by `Query<mut V, with Tag>`; too useful as identifiers to reserve |
| `component` stays the ECS declaration keyword | the engine's "attach a sub-object" helper was moved out of the keyword and into an ordinary function in `examples/turret.wo` |
| No turbofish | the lookahead scan is cheap and the ambiguous cases are rare |
| Struct literals are disabled in loop and condition subjects | `if ready { Fire() }` without parentheses is worth more than `if Weapon { ... }` |
| An expression nesting limit of 192 | a compiler must not blow its stack on hostile input; reported as `E0041` |

## Things worth remembering

- Parsing a declaration speculatively can leave orphan attribute nodes in the
  tree when the construct turns out not to be a declaration. They are
  unreachable and harmless, but a rollback mechanism would be needed if the
  parser ever grew real backtracking.
- `if const Point { x } = p` does not parse: struct patterns are disabled
  wherever struct literals are, which is the condition of `if`. Rare enough to
  defer.
- Doc comments attach to the declaration that follows them, including enum
  variants, and are kept as a merged span rather than text.

## Deferred to later waves

- `wolang fmt` moves to wave 3, where the printer gets a formatting mode rather
  than an s-expression one.
- Labelled `break` and `continue`, array and slice types (`[]T`, `[N]T`),
  prefix and suffix ranges (`..b`, `a..`), and `extern "C" { ... }` blocks.
- File-level attributes: `Ast` carries the field, but no syntax produces them.

## Next

Wave 3: the query database, modules and packages. See `STATUS.md`.
