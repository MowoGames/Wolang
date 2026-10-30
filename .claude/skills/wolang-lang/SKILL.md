---
name: wolang-lang
description: Writing or reading Wolang (.wo) code - syntax, effects, optionals/errors, std modules, @Test. Use before writing any .wo file, a library change under lib/, an example, or a doc snippet.
---

# Writing Wolang

Dense reference so you do not have to read the 13 KB tour. When unsure of an
exact rule, grep `docs/spec.md` (numbered sections: 1 lexical, 2 grammar,
3 declarations, 4 types, 5 effects, 6 units, 7 layout, 8+ drafts) or the std
source in `lib/std/src`. Every snippet in docs is compiled by CI
(`tools/package/check_docs.py`), so a wrong one fails.

## Shape of a file
```wolang
need std.io.{Print}          // imports: one module per `need`, names in braces
need std.list.{ListNew}

fun main() uses io, unsafe { // effects are declared and checked through the call graph
  const fixed = 42           // `const` immutable, `var` mutable; inferred types
  var list = ListNew<int>()
  list.Push(fixed)
  Print("{list.len} items")  // `{expr}` interpolates
}
```
No semicolons; statements end at newline. Functions, methods and types are
`PascalCase`; locals `snake_case`/lowercase. `fun F(x: int) -> int => x * 2`
for one-expression bodies. Closures: `|a, b| a < b`, capture by copy.

## Rules that bite
- **Effects**: `io alloc rand time net gpu unsafe panic`. Calling something with an effect
  requires declaring it. Many std calls (`ListNew`, `Sqrt`, `ArrayNew`, atomics) need `unsafe`.
  A function with no `uses` is pure.
- **No implicit conversions**; integer overflow **traps** (use `std.math` `WrappingAdd` etc.).
  Literals are arbitrary precision until they meet a type (D9). Sized ints: `u8..u64`, `s8..s64`;
  `int`, `float`, `bool`, `char`, `text`.
- **Optionals** `T?`: `null`, `if const x = F() { }`, `a ?? default`, pattern `n? => ..., null => ...`.
  No `Some`/`None` (D12).
- **Failures** `T!`: `error TooBig` declares one; `return TooBig`; errors are inferred (D13);
  `match r { Ok(v) => ..., TooBig => ... }`; `?` propagates. `Pawn?!` = optional or failing.
- **match** is exhaustive; arms are `Pattern => expr,` with trailing commas; enum variants `Shape.Circle(r)`.
- **struct** = value (copied); **class** = reference counted handle; `weak var` breaks cycles;
  `Owned<T>` frees at scope end; `arena { }` frees everything taken inside at block end.
- Methods live in the struct body or `extend T { }`; abilities: `ability A { fun F() -> text }`,
  `extend T as A { ... }`; associated types `type Item = int`, read as `T.Item`.
- Generics `fun F<T: Bound>(x: T)` are monomorphised; `fun F(x: Ability)` is dynamic dispatch.
- Loops: `for i in 0..5` (exclusive), `while`, `loop { break }`, labels `outer: for ...` / `break outer`.
  `foreach` is the SPMD/system loop over queries.
- Units: `dimension Length`, `unit m: Length`, `100m / 9.58s` is Length/Time.
- Gamedev: `component`, `tag`, `system S(dt: float, q: Query<mut A, B, with Tag>)`, `@After(S)`;
  `machine M { state Off { on Flip -> On } }`; `derived` fields; `soa []T` / `aos []T`.
- C: `extern "C" fun strlen(s: *u8) -> u64`; `ptrnull`; `&mut x`; `wolang bind header.h`.
- A result you mean to drop is bound to an underscore name: `const _held = F()`.

## Running a snippet
A lone `.wo` file resolves **no imports**: `need std...` fails (E0055) and even `Print` is
unknown (E0103) unless the file declares it (see `examples/hello.wo`). Use a package under the
gitignored `build/scratch/`:
```sh
.claude/scripts/wl wo new build/scratch/try     # manifest points std at lib/std
# edit build/scratch/try/src/main.wo, then
.claude/scripts/wl wo run build/scratch/try
```

## Tests
```wolang
@Test
fun AddsTwoNumbers() -> bool {
  return Add(2, 2) == 4
}
```
Library tests sit in the same file as the code. `wl lib std` runs them (in two tiers, compared).

## Tooling
`wl wo check <path>`, `wl wo run <file|dir>`, `wl wo run --interpret <file>` (reference tier),
`wl wo ir <file>`, `wl wo fmt --write <file>` (the PostToolUse hook already formats .wo you edit),
`wl wo test <dir>`, `wl wo doc lib/std -o out.md`. Std module list: `grep -n '^## ' docs/stdlib.md`.
