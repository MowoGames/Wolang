# game

## game.ecs

### class `Store`

```wolang
offer class Store<T>
```

The components of one type, packed.

Rows are dense: the component in row `i` belongs to entity `owners[i]`,
and a pass over the rows reads them in order with no holes to skip.
`slots[e]` is the row entity `e` occupies plus one, or zero when it has
none of this component -- plus one so that an unused slot is the zero a
fresh array already holds.

Removing a component moves the last row into the hole rather than
shifting the rest. That is what keeps the pass sequential, and it is
why nothing may hold a row index across a removal.

#### field `rows`

```wolang
var rows: soa []T
```

#### field `owners`

```wolang
var owners: []int
```

#### field `slots`

```wolang
var slots: []int
```

#### field `count`

```wolang
var count: int = 0
```

#### method `Row`

```wolang
fun Row(e: int) -> int
```

The row entity `e` occupies, or -1 when it has no component here.

#### method `Has`

```wolang
fun Has(e: int) -> bool
```

#### method `Add`

```wolang
fun Add(e: int) -> int
```

The row for `e`, adding one if it has none. -1 when the store is
full or the entity is outside the range it was built for.

#### method `Remove`

```wolang
fun Remove(e: int)
```

Drops `e`'s component by moving the last row into its place.

### function `StoreNew`

```wolang
offer fun StoreNew<T>(capacity: int, entities: int) -> Store<T> uses unsafe
```

A store for `capacity` components of up to `entities` entities.

Fixed size: there is no growth yet, so `Add` reports -1 rather than
reallocating. A game that knows its budget is the case this serves,
and growth is a copy loop away.

### class `Entities`

```wolang
offer class Entities
```

Hands out entity numbers.

Numbers are not reused yet. Reuse needs a generation counter beside the
index -- otherwise a handle to a dead entity reads the one that took
its place -- and that is a change to what an entity *is*, so it waits
until there is something to free.

#### field `next`

```wolang
var next: int = 0
```

#### field `limit`

```wolang
var limit: int = 0
```

#### method `Spawn`

```wolang
fun Spawn() -> int
```

A fresh entity, or -1 when there are no more.

#### method `Len`

```wolang
fun Len() -> int
```

### function `EntitiesNew`

```wolang
offer fun EntitiesNew(limit: int) -> Entities
```

### class `PackedStore`

```wolang
offer class PackedStore<T>
```

The components of one type, one record after another.

The same interface as `Store` and the other layout underneath: a pass
that touches most of a record wants this one, and a pass that touches
one field of a wide record wants the other. Which is the whole point of
the layout being a property of the storage (D73) -- the rows are the
only difference between the two, and a system reads them the same way.

#### field `rows`

```wolang
var rows: []T
```

#### field `owners`

```wolang
var owners: []int
```

#### field `slots`

```wolang
var slots: []int
```

#### field `count`

```wolang
var count: int = 0
```

#### method `Row`

```wolang
fun Row(e: int) -> int
```

#### method `Has`

```wolang
fun Has(e: int) -> bool
```

#### method `Add`

```wolang
fun Add(e: int) -> int
```

#### method `Remove`

```wolang
fun Remove(e: int)
```

### function `PackedStoreNew`

```wolang
offer fun PackedStoreNew<T>(capacity: int, entities: int) -> PackedStore<T> uses unsafe
```

## game.world

### struct `Token`

```wolang
offer struct Token<T>
```

The number that says which component type this is.

A struct rather than a bare `u64` because the type it names is what
makes it the right number: `TokenOf<Position>()` has type
`Token<Position>`, and that is how the compiler knows which token to
answer with.

#### field `value`

```wolang
var value: u64
```

### function `TokenOf`

```wolang
offer fun TokenOf<T>() -> Token<T>
```

The token of `T`. Answered by the compiler, so it has no body.

### class `World`

```wolang
offer class World
```

A world: entities, and a component array per type.

#### field `handle`

```wolang
var handle: u64 = 0
```

#### method `Spawn`

```wolang
fun Spawn() -> u64 uses unsafe
```

A new entity with no components. Never zero.

#### method `Despawn`

```wolang
fun Despawn(entity: u64) uses unsafe
```

Removes it and everything it had.

#### method `Alive`

```wolang
fun Alive(entity: u64) -> bool uses unsafe
```

#### method `Len`

```wolang
fun Len() -> int uses unsafe
```

How many entities are living.

#### method `Destroy`

```wolang
fun Destroy() uses unsafe
```

Gives the storage back. Not a `deinit`: a world is handed to the
functions that work on it, and a type with a destructor would be
moved by that rather than borrowed (D87).

### function `WorldNew`

```wolang
offer fun WorldNew() -> World uses unsafe
```

### function `Add`

```wolang
offer fun Add<T>(world: World, entity: u64, value: T) uses unsafe
```

Gives `entity` a component, or replaces the one it has.

### function `Get`

```wolang
offer fun Get<T>(world: World, entity: u64) -> T? uses unsafe
```

What `entity`'s component of `T` is, or nothing when it has none.

### function `Has`

```wolang
offer fun Has<T>(world: World, entity: u64, of: Token<T>) -> bool uses unsafe
```

Whether `entity` has the component `of` names.

The token is an argument rather than a type parameter of its own
because the answer is a `bool`: a type parameter has to appear in the
signature for a call to be able to say what it is (`E0133`), and here
the token is where it appears.

### function `Remove`

```wolang
offer fun Remove<T>(world: World, entity: u64) -> T? uses unsafe
```

Takes `T` away from `entity` and gives back what it was.

### function `CountOf`

```wolang
offer fun CountOf<T>(world: World, of: Token<T>) -> int uses unsafe
```

How many entities have the component `of` names.

### function `RunSystems`

```wolang
offer fun RunSystems(world: World, delta: f32) uses unsafe
```

Runs every `system` of the calling file, stage by stage.

What a stage is, is the compiler's answer: the systems in one may run
at the same time because none of them writes what another reads, and
the runtime starts a thread per system in it (D101). It has no body
for the same reason `TokenOf` has none -- the schedule is a property of
the declarations, and only the compiler can see them all.

The systems that run are the ones in the file that calls this: the
address of a function is something only the module holding it can
give. A program whose systems live in several files calls it once per
file.
