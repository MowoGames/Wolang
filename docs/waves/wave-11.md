# Wave 11 — a layout, an optimizer, and generics that cross a module

**Status:** complete for the parts that could be measured; the parallel
scheduler is carried forward. `soa []T` is in the language, the passes that
make it pay for itself are in the optimizer, generics work across module
boundaries — which is what an ECS written in Wolang needed — and `system`
declarations are scheduled into stages with conflicts rejected. What is not
here is a `Query` value built from a world, so a system body is checked and
scheduled but not run, and the stages are not handed to threads because the
runtime has none. 632 unit tests, all passing in Debug, sanitizers, Release
and under x86-64.

## Scope

The wave was "ECS, access graph, parallel scheduler, AoS/SoA layout
polymorphism, SPMD `foreach`", with the exit criterion "1M entity
integration benchmark; the SoA variant beats the AoS one".

It turned into four pieces, in the order the work forced:

1. `soa []T` — the layout, in every tier.
2. The optimizer, because the layout was invisible underneath the address
   arithmetic.
3. Generics across modules, because an ECS library needs `Store<T>` and a
   library nothing outside it can call is not a library.
4. `system` and the access graph.

## The layout

```wolang
const bodies = SoaNew<Body>(1000000)   // one array per field
const packed = ArrayNew<Body>(1000000) // one record after another
```

The same record, the same code reading it, the storage turned inside out
(D73). Each field gets an array of its own, and the count each array holds
is rounded up to sixteen — which is what makes the start of a field's array
a compile-time constant times one number, so an address costs one multiply
rather than one per field, and leaves every array sixteen-byte aligned for a
vector load later.

```
$ wolang bench benchmarks/entities
@BenchIntegrateSoa            25.262 ms   result 8000000
@BenchIntegrateAos            21.330 ms   result 8000000
@BenchSumOneFieldSoa          22.761 ms   result 16000000
@BenchSumOneFieldAos          23.675 ms   result 16000000
@BenchWideFieldSoa            20.650 ms   result 0
@BenchWideFieldAos            28.135 ms   result 0
```

Both directions, which is the honest answer. A pass over **one field of a
sixteen-field record** is 1.36x faster as `soa`: four megabytes of traffic
per million elements instead of sixty-four. A pass that touches **four
fields of six** is faster laid out whole, because then a cache line carries
mostly what the pass wanted anyway. The layout is a choice, and this is the
measurement that tells you which way to make it.

## The optimizer, because the layout was invisible

The first version of the SoA benchmark took 107 ms. Not because of the
layout — because of what the generated code did per element. Three passes
later it takes 25.

- **Common subexpressions, one copy per block** (D77). Reading a field,
  adding to it and writing it back asks for the same address twice.
- **Loop invariants, hoisted into the preheader** (D77). The base of a
  slice's storage, its length, the start of a field's array.
- **Element addresses expanded into arithmetic** (D76), so the two passes
  above can see inside them, with `index.check` left behind where it was —
  the part that is about *this* element.
- **Arithmetic the compiler wrote for itself is not checked** (D75), which
  is what makes it hoistable at all: moving a checked multiply out of a loop
  that never runs would trap where the program would not have.
- **A load is reused until something writes** (D81).

| benchmark | before | after |
|---|---|---|
| 1M entity integration, `soa` | 107.2 ms | 25.3 ms |
| 1M entity integration, packed | 64.1 ms | 21.3 ms |
| `BenchVectorDot` | 3.43 ms | 2.55 ms |

Three bugs came out of it, and two were mine rather than the compiler's: a
dangling span into a vector that was being appended to (which the x86-64
build turned into an infinite loop and the arm64 one tolerated), a
dominator chain walked before it existed, and a bounds check that
disappeared because the pass that removes dead code treated it as pure. A
bounds check that disappears is the kind of bug a benchmark reports as an
improvement.

## Generics that cross a module

An ECS store is `Store<T>` over `soa []T`, which needed three things the
language did not have:

- **A constructor binds its type arguments** (D78). `Box<int>(7)` builds a
  `Box<s32>`, and a field of it reads as `int` rather than as `T`. The
  layout was the thing that was wrong before: an instance was not a named
  type to it, so every field of a two-field instance sat at offset zero and
  reading one gave the other.
- **A call takes the type arguments it writes** (D74). Inference reads the
  arguments, and `SoaNew<Particle>(count)` has only a number to go on.
- **A generic is copied where it is used, with the names of where it was
  written** (D79). The module a generic lives in cannot know what its
  importers will ask for, so lowering borrows the defining file's syntax,
  member tables and recorded types for as long as it takes to lower the
  copy. `Store<T>`'s body resolves `SoaNew` in the file that wrote it.

The reference tier also learned to read and write aggregates through a
pointer (D80) — a class holding a slice has a sixteen-byte field, and it had
no way to write one at all.

## The ECS, in Wolang

`lib/game/src/ecs/ecs.wo`: a dense `Store<T>` over `soa []T`, a
`PackedStore<T>` over `[]T` with the same interface, and entity numbers.
Rows are packed, `slots[e]` is the row an entity occupies plus one, and a
removal moves the last row into the hole so a pass stays sequential. Four
`@Test` functions, passing in both tiers.

```
$ wolang bench benchmarks/ecs
@BenchEcsIntegrateSoa         43.283 ms   result 8000000
@BenchEcsIntegrateAos         44.718 ms   result 8000000
```

A million entities, eight rounds, through the stores: the `soa` variant is
3% faster. That is the wave's criterion met, and narrowly — the loop is
still compute-bound rather than memory-bound, so most of the layout's
advantage is not visible yet. What is left in the way is hoisting a *load*
out of a loop, which needs to know that nothing the loop writes lands where
the load reads. That is alias analysis, and it is the next thing worth
building for the dev tier (D81).

## Systems and the access graph

```
$ wolang systems examples/systems
module systems
    system ApplyGravity(mut Velocity, Falling)
    system Integrate(mut Position, Velocity)
    stage 1: ApplyGravity
    stage 2: Integrate
```

A system declares its access in its signature and the schedule follows
(D82): everything in a stage may run at once, because nothing in it writes
what another touches. Two systems that would share a stage and conflict are
rejected (`E0127`) rather than ordered arbitrarily — the arbitrary order is
the bug a scheduler like this exists to prevent. `@After` and `@Before` are
the same edge written from the two ends; an ordering that names nothing is
`E0128` and a cycle is `E0129`.

The graph is a module-level query, so two systems in different files are
scheduled against each other, and an edit to one rebuilds only what depends
on the answer.

## What is carried forward

- **A `Query` value built from a world.** The storage is a library, the
  schedule is the compiler's, and the join between them is the next piece:
  which store a query reads, and how a stage is handed to a thread.
- **The parallel scheduler.** The stages are computed; running them at the
  same time needs threads in the runtime, which do not exist yet.
- **SPMD `foreach`.** The loop runs one element at a time; mapping it onto
  lanes waits for vectorization in the back end.
- **Alias analysis**, so a loop-invariant load can be hoisted. The largest
  single thing between the dev tier and the layout difference showing up as
  more than a few percent.
- **Growth in a store.** `Add` reports -1 when full rather than
  reallocating.

## What the wave taught

The measurement that mattered was not the one the criterion asked for. "The
SoA variant beats the AoS one" was true at 3% and could have been declared
done; what the benchmark actually said was that the generated code was
spending seventeen cycles per element on arithmetic the optimizer should
have removed. Fixing that was worth four times more than the layout, and it
made every other benchmark faster too.

The other lesson is the old one, twice: both of the hardest bugs this wave
were undefined behaviour in my own C++ that one target tolerated and the
other did not. Running the whole suite under a second architecture is worth
what it costs.
