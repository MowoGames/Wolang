# Wave 6 — the typed SSA IR

**Status:** complete. 485 unit tests, all passing in Debug, sanitizers and
Release.

## Scope

Give the compiler an intermediate representation, lower the typed tree into
it, verify it, and optimize it. The exit criterion was that the IR dump is
stable and the verifier is green on every test input — both of which it now
is, and the second is enforced rather than promised: lowering runs the
verifier on every file, twice.

## What was built

### The representation — `include/wolang/ir/ir.h`

Four decisions shape it, and each one is a reaction to a way the obvious
design goes wrong.

**Block parameters instead of phi nodes.** A value that depends on which edge
was taken is a parameter of the block, and the jump passes it. A phi node
carries the same information written where it is easy to get wrong; as a
parameter it becomes an arity and type check the verifier already performs.
`VerifierTest.RejectsAUseThatIsNotDominated` is the bug this makes
impossible to miss, and the test right after it is the fix.

**One id space for instructions and values.** An instruction *is* the value
it produces, so "where did this come from" is an index rather than a search.

**No renumbering, ever.** A dead instruction becomes an `undef` nothing uses;
a removed block is emptied rather than erased. An id is an index, and
compacting would mean rewriting every operand in the function. The verifier
and the printer both treat an empty block as deleted, which is what keeps
that representable.

**Flat arrays with side pools**, the same layout the AST uses. `Builder`
keeps the four pools consistent with the instructions that index into them.

The opcode table is an x-macro (`ir_opcode.inc`), so the mnemonic table, the
enum and the terminator predicate cannot drift apart.

### Erasure, and the one exception

Dimensions and open literal types are checking-time notions and neither
exists in the IR: `Length` is `f32`, `Length/Time` is `f32`, and a bare `1.0`
has settled on its default. Erasing once, at the lowering boundary, is what
makes "unit safe code costs nothing at runtime" true rather than aspirational.

Effect rows are **kept**. This was the open question wave 5 left, and the
answer is that "this call does nothing observable" is exactly what licenses
deleting it as dead — so the information has to survive into the IR.
`PassTest.KeepsACallWithEffects` is that answer as a test.

### Lowering — `src/ir/lower.cc`

Lowering trusts the checker: it reads recorded types rather than re-deriving
them, and it only runs on a file that produced no errors. That division keeps
the IR free of "maybe" cases the back ends would inherit.

| Construct | Becomes |
|---|---|
| `if`, `while`, `loop`, labels | blocks, with a parameter on the join when the `if` produces a value |
| `&&`, `\|\|`, `??` | control flow, because the right side may not run |
| `match` | arms tested in order, ending in `unreachable` where the checker proved coverage |
| `T?` | `opt.present`, `opt.value`, `opt.wrap`, `const.null` |
| `if let` | a presence test, with the inner value bound for the then-branch |
| a `var` | an `alloca` with loads and stores |
| a field | `extract` by index, from the member table's declaration order |
| a constructor | `struct`, fields in order |

A declaration with no body — a trait method, an `extern`, a host function —
has a signature and no blocks. Everything that walks blocks says so rather
than assuming an entry exists, which is what `Function::is_declaration()` is
for.

A call into another file gets a signature-only entry in the module, the same
shape a trait method has.

### The verifier — `src/ir/verifier.cc`

Runs on every input, not on request: the IR is the contract between the front
end and every back end, so a violation has to surface where it was
introduced. It checks block shape, dominance, edge arity and types,
reachability, operand types, and a call against its callee's signature.

Dominance uses the iterative set algorithm rather than Lengauer-Tarjan.
Slower, and far easier to be sure of — the right trade for something that
runs on every test input and has to be correct rather than fast.

The IR is verified **twice**, as lowering left it and after the passes, with
the messages prefixed accordingly. A pass that breaks an invariant and a
lowering that breaks one are different bugs.

### Passes — `src/ir/passes.cc`

| Pass | Does |
|---|---|
| `FoldConstants` | arithmetic and comparison on two constants, plus `lnot` and `neg` |
| `EliminateDeadCode` | drops what nothing reads and nothing can observe |
| `PromoteSlots` | a slot with one store, dominating every load, becomes the value |
| `SimplifyControlFlow` | a branch on a constant becomes a jump; unreachable blocks go |

`FoldConstants` does **not** fold division by zero: a fault at runtime is the
right answer and a wrong constant is not.

`PromoteSlots` handles the single-store case and deliberately leaves the rest
alone — promoting a slot written on several paths needs block parameters
inserted at the joins, which is the pass's next step.
`PassTest.LeavesASlotWrittenOnSeveralPathsAlone` pins that honestly rather
than pretending the pass is finished.

### The driver

`wolang ir <paths>` prints the IR and sends verifier complaints to stderr
with exit code 70, keeping the dump on stdout — a bug report needs both.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| Effect rows go into the IR rather than being erased | "does nothing observable" is what licenses dead-code removal of a call |
| Block parameters, not phi nodes | the same information where the verifier already checks it |
| A pass never renumbers | an id is an index; compacting means rewriting every operand |
| An empty block is a deleted block | removal has to be representable without renumbering |
| The verifier runs on every input, twice | the IR is a contract, and a pass bug must be distinguishable from a lowering bug |
| Dominance by iterative sets | correctness over speed in the one component that must not be wrong |
| Lowering only runs on a file with no errors | otherwise every value carries a "maybe" into every back end |

## Bugs this wave found

- **A second `wolang::Module`.** The IR named its module struct the same as
  the existing one for a module of source files. Two structs with one
  fully-qualified name is an ODR violation: the linker picks a definition,
  code compiled against the other reads the wrong offsets, and the symptom
  was memory corruption in an unrelated test. The debug build was green; the
  sanitizer preset caught it. The IR now lives in `wolang::ir`, and
  `docs/style.md` says why a layer with generic vocabulary gets a namespace.
- **`Builder` held a `Function*`** into the module's function vector.
  Declaring an imported callee grew that vector and the pointer dangled. It
  holds an index now.
- **A loop's body is in `rhs`, not `lhs`.** Lowering read the wrong field for
  `loop`, producing an empty body — and the verifier caught it as an
  unreachable exit block rather than letting it through.
- **An `if` that produced a value lowered its then-branch twice**, once
  through `Value` and once through `Lower`.
- **The printer crashed on a declaration**, calling `ParamsOf` on a function
  with no blocks.
- **A unit literal kept its dimension into the IR**, so `2m / 1s` tried to
  divide two different types. The verifier reported it before anything else
  could.
- **`ArgumentsOf` asserted on an invalid type.** Asking for the arguments of
  a type that is not there is a legitimate "none" — a declaration whose
  signature never resolved still gets walked.
- **A `match` allocated a test block for its last arm**, which stayed empty.

Six of the eight were found by the verifier or the sanitizer rather than by a
failing expectation, which is the point of both.

## Things worth remembering

- `CheckedFile` carries the *merged* resolution now. Lowering needs the same
  view the checker had: an imported function's signature only exists after
  the merge, and the un-merged one gave every imported callee an empty
  parameter list.
- The dump prints a result type only where no operand implies one —
  `alloca`, `undef`, `const.null`, `struct`, `array`. Printing every type
  would double the text that golden tests and wave 12's differential replay
  have to compare.
- `IsPure` treats `alloca` as impure, so DCE needs a separate rule for a slot
  nothing reads. Without it, promotion would leave behind every `alloca` it
  had just made unnecessary.

## Deferred to later waves

- `PromoteSlots` for a slot written on several paths, which needs block
  parameters inserted at joins.
- Inlining. The call graph is there and the effect rows make the decision
  informable, but it belongs with the cost model in wave 8.
- An IR parser. The printer is stable and `docs/architecture.md` describes
  the form; reading it back is what wave 12's differential fuzzing needs, not
  wave 6.
- String interpolation lowers to `undef`: building a string at runtime needs
  the formatting traits from `core`, which is wave 7.
- Destructuring a `let` lowers the initializer and drops the pattern.
- `for`, `foreach`, `defer`, `async` and the entity constructs are not
  lowered yet; each belongs with the wave that gives it semantics.

## Next

Wave 7: the IR interpreter — the reference semantics every later tier is
checked against — `comptime` evaluation, `wolang run`, and the first `core`
modules written in Wolang. See `STATUS.md`.
