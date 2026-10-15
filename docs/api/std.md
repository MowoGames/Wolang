# std

## std.env

### function `ArgCount`

```wolang
offer fun ArgCount() -> int uses unsafe
```

How many arguments the program was given.

### function `ArgAt`

```wolang
offer fun ArgAt(index: int) -> text uses unsafe
```

Argument `index`, counting from zero, or "".

### function `Lookup`

```wolang
offer fun Lookup(name: text) -> text? uses unsafe
```

The environment variable called `name`, or nothing.

An optional rather than "" because a variable set to nothing and one
that is not there mean different things to a program reading
configuration.

### function `Get`

```wolang
offer fun Get(name: text, fallback: text) -> text uses unsafe
```

The environment variable called `name`, or `fallback`.

### function `Set`

```wolang
offer fun Set(name: text, value: text) -> bool uses unsafe
```

Sets one, for this program and whatever it starts. True on success.

## std.fs

### class `File`

```wolang
offer class File
```

An open file. Closing it is `deinit`'s job, so a handle that goes out
of scope leaves no descriptor behind.

#### field `fd`

```wolang
var fd: s32 = -1
```

#### method `deinit`

```wolang
deinit
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Close`

```wolang
fun Close() uses unsafe
```

#### method `Read`

```wolang
fun Read(buffer: []u8) -> int uses unsafe
```

Fills `buffer` and says how many bytes arrived: zero at the end of
the file, negative when the system refused.

#### method `Write`

```wolang
fun Write(bytes: []u8) -> int uses unsafe
```

### function `Open`

```wolang
offer fun Open(path: text) -> File uses unsafe
```

Opens `path` for reading. The handle is closed when it goes away.

### function `Create`

```wolang
offer fun Create(path: text) -> File uses unsafe
```

Opens `path` for writing, making it or emptying it first.

### function `ReadAll`

```wolang
offer fun ReadAll(path: text, limit: int) -> []u8 uses unsafe
```

Reads the whole of `path` into fresh storage.

The caller owns what comes back and gives it to `Free` -- which is
what a `[]u8` is: a view of bytes that are somewhere, and here the
somewhere is an allocation.

### function `WriteAll`

```wolang
offer fun WriteAll(path: text, bytes: []u8) -> bool uses unsafe
```

Writes `bytes` to `path`, replacing what was there.

## std.io

### function `Write`

```wolang
offer fun Write(message: text) uses io, unsafe
```

Writes `text` to standard output, with no line break.

### function `Print`

```wolang
offer fun Print(message: text) uses io
```

Writes `text` and a line break.

## std.json

### function `Kind`

```wolang
offer fun Kind(value: Json) -> int
```

Which of the six a value is, as a number: `0` null, `1` bool, `2`
number, `3` text, `4` array, `5` object.

For a program that switches on the shape of a document it did not
write, which is most of what reading JSON is.

### error `Malformed`

```wolang
offer error Malformed
```

What a document that is not one answers with.

### enum `Json`

```wolang
offer enum Json
```

One of the six things a JSON value can be.

#### method `Null`

```wolang
Null
```

#### method `Bool`

```wolang
Bool(value: bool)
```

#### method `Number`

```wolang
Number(value: float)
```

#### method `Text`

```wolang
Text(value: text)
```

#### method `Array`

```wolang
Array(items: List<Json>)
```

#### method `Object`

```wolang
Object(fields: List<Field>)
```

### struct `Field`

```wolang
offer struct Field
```

A member of an object: its name and its value.

#### field `name`

```wolang
var name: text
```

#### field `value`

```wolang
var value: Json
```

### function `Parse`

```wolang
offer fun Parse(input: text) -> Json! uses unsafe
```

Reads one value from `input`.

### function `Write`

```wolang
offer fun Write(value: Json) -> text uses unsafe
```

`value` as JSON text.

### function `Member`

```wolang
offer fun Member(value: Json, name: text) -> Json? uses unsafe
```

The member of an object called `name`, or nothing.

### function `At`

```wolang
offer fun At(value: Json, at: int) -> Json? uses unsafe
```

Element `at` of an array, or nothing.

### function `Len`

```wolang
offer fun Len(value: Json) -> int uses unsafe
```

How many elements an array has, or how many members an object has.

### function `AsNumber`

```wolang
offer fun AsNumber(value: Json, fallback: float) -> float
```

The number this is, or `fallback`.

### function `AsInt`

```wolang
offer fun AsInt(value: Json, fallback: int) -> int
```

The whole number this is, or `fallback`.

### function `AsText`

```wolang
offer fun AsText(value: Json, fallback: text) -> text
```

The string this is, or `fallback`.

### function `AsBool`

```wolang
offer fun AsBool(value: Json, fallback: bool) -> bool
```

The boolean this is, or `fallback`.

### function `IsNull`

```wolang
offer fun IsNull(value: Json) -> bool
```

### function `Object`

```wolang
offer fun Object() -> Json uses unsafe
```

An empty object, to put members in.

### function `Array`

```wolang
offer fun Array() -> Json uses unsafe
```

An empty array, to put values in.

### function `Put`

```wolang
offer fun Put(object: Json, name: text, value: Json) -> Json uses unsafe
```

Puts a member in an object. The object is the value it answers with,
so calls chain: `Put(Put(Object(), "a", ..), "b", ..)`.

### function `Push`

```wolang
offer fun Push(array: Json, value: Json) -> Json uses unsafe
```

Adds a value to an array.

### function `Number`

```wolang
offer fun Number(value: float) -> Json =>
```

### function `OfInt`

```wolang
offer fun OfInt(value: int) -> Json =>
```

### function `OfText`

```wolang
offer fun OfText(value: text) -> Json =>
```

### function `OfBool`

```wolang
offer fun OfBool(value: bool) -> Json =>
```

### function `Null`

```wolang
offer fun Null() -> Json =>
```

## std.list

### class `List`

```wolang
offer class List<T>
```

A sequence that grows.

Doubling, and never shrinking: an append is amortised constant and a
list that was big once stays ready to be big again, which is what a
frame loop wants. `deinit` gives the storage back, so a list that goes
out of scope leaves nothing behind.

#### field `items`

```wolang
var items: []T
```

#### field `count`

```wolang
var count: int = 0
```

#### method `deinit`

```wolang
deinit
```

#### method `Len`

```wolang
fun Len() -> int
```

#### method `Iterate`

```wolang
fun Iterate() -> ListWalk<T>
```

What `for value in list` walks with.

#### method `Capacity`

```wolang
fun Capacity() -> int
```

#### method `At`

```wolang
fun At(index: int) -> T
```

The element at `index`, or the first one when the index is outside
the list. Reading past the end is a mistake the bounds check would
catch; this is the total function the library offers instead.

#### method `Set`

```wolang
fun Set(index: int, value: T)
```

#### method `Push`

```wolang
fun Push(value: T) uses unsafe
```

Adds `value` to the end, growing the storage when it is full.

#### method `Pop`

```wolang
fun Pop() -> T
```

Drops the last element and gives it back, or the first one when the
list is empty.

#### method `Clear`

```wolang
fun Clear()
```

#### method `Reserve`

```wolang
fun Reserve(wanted: int) uses unsafe
```

Makes room for at least `wanted` elements.

Doubling rather than growing by one: a push that copies every time
turns a loop into quadratic work, and the whole reason to have a list
is that it does not.

### class `ListWalk`

```wolang
offer class ListWalk<T>
```

Walks a list from front to back.

`Next` moves and says whether there is anything there; `Value` is what
it is standing on. Three names and no ability, because an ability that
said "what I walk over" needs an associated type the language does not
have yet (D96).

#### field `over`

```wolang
var over: List<T>
```

#### field `at`

```wolang
var at: int = -1
```

#### method `Next`

```wolang
fun Next() -> bool
```

#### method `Value`

```wolang
fun Value() -> T
```

### function `ListNew`

```wolang
offer fun ListNew<T>() -> List<T> uses unsafe
```

An empty list, with no storage until something is pushed.

## std.map

### class `Map`

```wolang
offer class Map<K, V>
```

A map from `K` to `V`.

Open addressing with linear probing and a load factor of a half: one
allocation per table rather than one per entry, which is what makes a
lookup a couple of cache lines instead of a pointer chase. `deinit`
gives the three arrays back.

#### field `keys`

```wolang
var keys: []K
```

#### field `values`

```wolang
var values: []V
```

#### field `filled`

```wolang
var filled: []bool
```

#### field `count`

```wolang
var count: int = 0
```

#### field `hash`

```wolang
var hash: fun(K) -> int
```

#### field `equals`

```wolang
var equals: fun(K, K) -> bool
```

#### method `deinit`

```wolang
deinit
```

#### method `Len`

```wolang
fun Len() -> int
```

#### method `Iterate`

```wolang
fun Iterate() -> MapWalk<K, V>
```

What `for key in map` walks with: the keys, and `Get` for the rest.

#### method `Slot`

```wolang
fun Slot(key: K) -> int
```

Where `key` is, or where it would go. -1 when there is no room,
which cannot happen while the load factor is kept under one.

#### method `Has`

```wolang
fun Has(key: K) -> bool
```

#### method `Get`

```wolang
fun Get(key: K, fallback: V) -> V
```

The value for `key`, or `fallback` when it is not there. A total
function rather than an optional, because `V` may be anything and an
optional of it would be a second allocation shape to lay out.

#### method `Put`

```wolang
fun Put(key: K, value: V) uses unsafe
```

#### method `Remove`

```wolang
fun Remove(key: K) -> bool
```

Takes `key` out. Answers whether it was there.

By shifting the run that follows back rather than by leaving a
tombstone: a probe stops at the first empty slot, so a hole in the
middle of a run would hide everything after it. Shifting keeps the
invariant that every key is reachable from where it hashes to,
which is what makes a lookup a walk and not a search.

#### method `Reserve`

```wolang
fun Reserve(wanted: int) uses unsafe
```

Makes room for `wanted` entries, keeping the table at most half
full: linear probing degrades sharply past that, and the whole
reason to probe rather than chain is that it does not chase
pointers.

### class `MapWalk`

```wolang
offer class MapWalk<K, V>
```

Walks the entries of a map, in whatever order the table holds them.

A map is a table with holes, so walking it means skipping them: that
is why this is a class with a position rather than an index into
anything the caller can see.

#### field `over`

```wolang
var over: Map<K, V>
```

#### field `at`

```wolang
var at: int = -1
```

#### method `Next`

```wolang
fun Next() -> bool
```

#### method `Value`

```wolang
fun Value() -> K
```

### function `MapNew`

```wolang
offer fun MapNew<K, V>(hash: fun(K) -> int, equals: fun(K, K) -> bool) -> Map<K, V> uses unsafe
```

### function `IntHash`

```wolang
offer fun IntHash(key: int) -> int
```

Hashing and comparing for `int` keys, which is most of them.

### function `IntEquals`

```wolang
offer fun IntEquals(left: int, right: int) -> bool
```

## std.math

### constant `Pi`

```wolang
offer const Pi: float = 3.141592653589793
```

The ratio of a circle's circumference to its diameter.

### constant `Tau`

```wolang
offer const Tau: float = 6.283185307179586
```

A whole turn, which is the one that comes up in gameplay code.

### function `Sqrt`

```wolang
offer fun Sqrt(x: float) -> float uses unsafe
```

### function `Sin`

```wolang
offer fun Sin(x: float) -> float uses unsafe
```

### function `Cos`

```wolang
offer fun Cos(x: float) -> float uses unsafe
```

### function `Tan`

```wolang
offer fun Tan(x: float) -> float uses unsafe
```

### function `Atan2`

```wolang
offer fun Atan2(y: float, x: float) -> float uses unsafe
```

The angle of the vector `(x, y)`, in radians.

### function `Pow`

```wolang
offer fun Pow(x: float, y: float) -> float uses unsafe
```

### function `Exp`

```wolang
offer fun Exp(x: float) -> float uses unsafe
```

### function `Log`

```wolang
offer fun Log(x: float) -> float uses unsafe
```

The natural logarithm.

### function `Floor`

```wolang
offer fun Floor(x: float) -> float uses unsafe
```

### function `Ceil`

```wolang
offer fun Ceil(x: float) -> float uses unsafe
```

### function `Round`

```wolang
offer fun Round(x: float) -> float uses unsafe
```

### function `Mod`

```wolang
offer fun Mod(x: float, y: float) -> float uses unsafe
```

The remainder of `x / y`, keeping `x`'s sign.

### function `AbsFloat`

```wolang
offer fun AbsFloat(x: float) -> float
```

### function `Abs`

```wolang
offer fun Abs(x: int) -> int
```

### function `Min`

```wolang
offer fun Min(a: int, b: int) -> int
```

### function `Max`

```wolang
offer fun Max(a: int, b: int) -> int
```

### function `MinFloat`

```wolang
offer fun MinFloat(a: float, b: float) -> float
```

### function `MaxFloat`

```wolang
offer fun MaxFloat(a: float, b: float) -> float
```

### function `Clamp`

```wolang
offer fun Clamp(value: int, low: int, high: int) -> int
```

`value`, kept between `low` and `high`.

### function `ClampFloat`

```wolang
offer fun ClampFloat(value: float, low: float, high: float) -> float
```

### function `Lerp`

```wolang
offer fun Lerp(a: float, b: float, t: float) -> float
```

`a` and `b` mixed by `t`, which is the one every gameplay curve is
made of.

### function `Near`

```wolang
offer fun Near(a: float, b: float, tolerance: float) -> bool
```

Whether two floats are within `tolerance` of each other.

Comparing floats with `==` is almost always a bug; this is what the
comparison usually meant.

### function `WrappingAdd`

```wolang
offer fun WrappingAdd(a: u64, b: u64) -> u64
```

`a + b`, keeping the low bits instead of trapping.

Everything else in this language traps on overflow, which is right for
a program counting damage and wrong for one hashing a key or stepping
a generator (D107). These three have no body: what they stand for is
the instruction the compiler already writes for its own arithmetic.

### function `WrappingSub`

```wolang
offer fun WrappingSub(a: u64, b: u64) -> u64
```

`a - b`, keeping the low bits.

### function `WrappingMul`

```wolang
offer fun WrappingMul(a: u64, b: u64) -> u64
```

`a * b`, keeping the low bits.

## std.mem

### function `BytesFromRaw`

```wolang
offer fun BytesFromRaw(data: *u8, length: int) -> []u8 uses unsafe
```

A view of `length` bytes at `data`.

`unsafe` because nothing here can check that there are that many, or
that they stay there. The caller promises both.

### function `SoaNew`

```wolang
offer fun SoaNew<T>(count: int) -> soa []T uses unsafe
```

Storage for `count` records, laid out as one array per field.

`soa []T` is the same record with a different layout: a walk over one
field touches every value in a cache line instead of every seventh.
Which is the whole point, and why this is a layout rather than a type.

Like `BytesFromRaw`, it has no body: how big the storage is, is a
question only a tier can answer, so the compiler answers it at the call.

### function `ArrayNew`

```wolang
offer fun ArrayNew<T>(count: int) -> []T uses unsafe
```

Storage for `count` records, one after another.

The same records as `SoaNew` and a different layout: this is the one
that reads well when a pass touches most of a record, and the other is
the one that reads well when it touches one field of it.

### function `Free`

```wolang
offer fun Free<T>(items: []T) uses unsafe
```

Gives back storage `ArrayNew` or `SoaNew` handed out.

`unsafe` because nothing here can check that the view is the whole
allocation, or that nothing else is still looking at it. A collection
that owns its storage calls this from its `deinit`, which is where
those two promises are easy to keep.

Like the two above, it has no body: what a view's storage *is* depends
on how the tier laid it out, so the compiler answers at the call.

### function `ViewFromRaw`

```wolang
offer fun ViewFromRaw<T>(data: *u8, count: int) -> []T uses unsafe
```

A view of `count` values of `T` at `data`.

`BytesFromRaw` with an element type: the same pair, and the same
reason for having no body. `unsafe` because nothing here can check
that there are that many values there, or that they stay.

### function `SizeOf`

```wolang
offer fun SizeOf<T>(value: T) -> int
```

How many bytes a value of `value`'s type takes.

The value is not read -- what is wanted is its type, which is why this
is spelled with an argument. How big a value is depends on how the
tier laid it out, so the compiler answers at the call (D100).

## std.own

### class `Owned`

```wolang
offer class Owned<T>
```

One value on the heap, owned by one name.

Moving it hands the value over; the name it was moved out of no longer
holds one, which the checker enforces (`E0122`). What it holds is
freed when it goes out of scope.

#### field `at`

```wolang
var at: *mut u8
```

#### field `live`

```wolang
var live: bool = true
```

#### method `deinit`

```wolang
deinit
```

#### method `Get`

```wolang
fun Get() -> T uses unsafe
```

What it holds.

#### method `Set`

```wolang
fun Set(value: T) uses unsafe
```

Replaces what it holds.

#### method `Release`

```wolang
fun Release() -> *mut u8 uses unsafe
```

Gives up ownership without freeing, and answers where the value is.

`unsafe` because nothing here can promise that something else will
free it. For handing a value to C, which then owns it.

### function `OwnedNew`

```wolang
offer fun OwnedNew<T>(value: T) -> Owned<T> uses unsafe
```

`value`, moved onto the heap.

### class `Shared`

```wolang
offer class Shared<T>
```

One value on the heap with several handles on it.

`Clone` takes another handle and the last one out frees the value.
There is no implicit copy: a type with a destructor is moved rather
than copied, so taking another handle is something the program says.

#### field `at`

```wolang
var at: *mut u8
```

#### field `live`

```wolang
var live: bool = true
```

#### method `deinit`

```wolang
deinit
```

#### method `Get`

```wolang
fun Get() -> T uses unsafe
```

What it holds.

#### method `Set`

```wolang
fun Set(value: T) uses unsafe
```

Replaces what it holds. Every handle sees the new value, which is
what sharing means.

#### method `Clone`

```wolang
fun Clone() -> Shared<T> uses unsafe
```

Another handle on the same value.

#### method `Count`

```wolang
fun Count() -> int uses unsafe
```

How many handles there are on the value.

### function `SharedNew`

```wolang
offer fun SharedNew<T>(value: T) -> Shared<T> uses unsafe
```

`value`, moved onto the heap with one handle on it.

## std.random

### class `Random`

```wolang
offer class Random
```

A generator. Two words of state, held by value.

#### field `low`

```wolang
var low: u64 = 0
```

#### field `high`

```wolang
var high: u64 = 0
```

#### method `Next`

```wolang
fun Next() -> u64 uses unsafe
```

The next 64 bits.

#### method `Below`

```wolang
fun Below(limit: int) -> int uses unsafe
```

A number in `0 .. limit`, with `limit` itself excluded.

Zero when the limit is not positive, which is the answer that keeps
a caller's loop from running forever.

#### method `Between`

```wolang
fun Between(from: int, to: int) -> int uses unsafe
```

A number from `low` to `high`, both included.

#### method `Unit`

```wolang
fun Unit() -> float uses unsafe
```

A float in `0.0 .. 1.0`.

#### method `Range`

```wolang
fun Range(from: float, to: float) -> float uses unsafe
```

A float from `from` to `to`.

#### method `Chance`

```wolang
fun Chance(chance: float) -> bool uses unsafe
```

True with probability `chance`.

### function `RandomNew`

```wolang
offer fun RandomNew(seed: u64) -> Random uses unsafe
```

A generator from a seed. The same seed gives the same sequence, which
is what makes a bug in a procedural level reproducible.

### function `RandomFromClock`

```wolang
offer fun RandomFromClock() -> Random uses unsafe
```

A generator seeded from the clock.

## std.set

### class `Set`

```wolang
offer class Set<T>
```

A set of `T`, with the hash and the equality the caller gives.

#### field `held`

```wolang
var held: Map<T, bool>
```

#### method `Len`

```wolang
fun Len() -> int
```

How many values it holds.

#### method `Add`

```wolang
fun Add(value: T) -> bool uses unsafe
```

Puts `value` in. Answers whether it was new.

#### method `Has`

```wolang
fun Has(value: T) -> bool uses unsafe
```

Whether `value` is in it.

#### method `Remove`

```wolang
fun Remove(value: T) -> bool uses unsafe
```

Takes `value` out. Answers whether it was there.

### function `SetNew`

```wolang
offer fun SetNew<T>(hash: fun(T) -> int, equals: fun(T, T) -> bool) -> Set<T> uses unsafe
```

## std.sort

### function `Sort`

```wolang
offer fun Sort<T>(items: []T, before: fun(T, T) -> bool)
```

Sorts `items` in place, with `before(a, b)` saying whether `a` comes
first.

The comparison has to be a strict order: `before(a, a)` false, and
`before(a, b)` and `before(b, a)` never both true. A comparison that
is not one leaves the elements in some order rather than looping.

### function `LowerBound`

```wolang
offer fun LowerBound<T>(items: []T, value: T, before: fun(T, T) -> bool) -> int
```

Where `value` is in a sorted slice, or where it would go.

The answer is an index between zero and `items.len`, which is what
makes it useful for inserting as well as for finding.

### function `Contains`

```wolang
offer fun Contains<T>(items: []T, value: T, before: fun(T, T) -> bool) -> bool
```

Whether a sorted slice holds `value`.

## std.tasks

### function `Yield`

```wolang
offer task fun Yield()
```

Gives the scheduler a chance to run something else.

### function `Run`

```wolang
offer fun Run() -> s64 uses unsafe
```

Runs every spawned task until none can make progress. Returns how many
steps that took.

This is where the effect row stops being able to help: what `Run` does
is whatever the tasks do, and the row cannot say so. Keep the spawning
close to the running and the question stays answerable by reading.

### function `Done`

```wolang
offer fun Done(handle: s32) -> bool uses unsafe
```

Whether the task behind `handle` has finished.

### function `Pending`

```wolang
offer fun Pending() -> s32 uses unsafe
```

How many spawned tasks have not finished.

### function `Join`

```wolang
offer fun Join(handle: s32) -> s64 uses unsafe
```

Runs until the task behind `handle` has finished.

Not a `task` itself, and that is a limitation rather than a design: a
`wait` on a task from another module needs the frame layout that module
computed (`E0305`), so joining is done by running the scheduler rather
than by suspending. Inside a task, prefer `wait` on something in your
own module and keep `Join` for the top level.

## std.text

### function `Length`

```wolang
offer fun Length(value: text) -> int
```

How many bytes a piece of text is. Not how many characters: UTF-8 is
bytes, and the two numbers differ exactly where it matters.

### function `IsEmpty`

```wolang
offer fun IsEmpty(value: text) -> bool
```

### function `Bytes`

```wolang
offer fun Bytes(value: text) -> []u8 uses unsafe
```

The bytes of `value`, as a view. Nothing is copied.

### function `Equals`

```wolang
offer fun Equals(left: text, right: text) -> bool uses unsafe
```

Whether the two hold the same bytes.

### function `StartsWith`

```wolang
offer fun StartsWith(value: text, prefix: text) -> bool uses unsafe
```

Whether `value` starts with `prefix`.

### function `IndexOf`

```wolang
offer fun IndexOf(value: text, needle: text) -> int uses unsafe
```

Where `needle` first appears in `value`, or -1.

### class `Builder`

```wolang
offer class Builder
```

Text that owns its bytes.

Appending copies into storage the builder grew, so what comes out of
`Text()` stays valid until the builder goes away -- which is what a
destructor is for, and why this is a class and not a function.

#### field `bytes`

```wolang
var bytes: []u8
```

#### field `count`

```wolang
var count: int = 0
```

#### method `deinit`

```wolang
deinit
```

#### method `Len`

```wolang
fun Len() -> int
```

#### method `Text`

```wolang
fun Text() -> text uses unsafe
```

What has been built, as a view of the builder's own bytes.

The view lasts as long as the builder does: appending more may move
the bytes, and the destructor gives them back. A string that has to
outlive the builder is `Release`.

#### method `Release`

```wolang
fun Release() -> text uses unsafe
```

What has been built, as bytes that outlive the builder.

The storage goes with the string and the builder starts again empty
-- which is what a function that *returns* what it built needs, and
what `Text` cannot give. Nothing frees the bytes yet: the same
latitude every string that is built takes today (D67).

#### method `Clear`

```wolang
fun Clear()
```

#### method `Append`

```wolang
fun Append(value: text) uses unsafe
```

#### method `AppendByte`

```wolang
fun AppendByte(value: u8) uses unsafe
```

#### method `Reserve`

```wolang
fun Reserve(wanted: int) uses unsafe
```

### function `BuilderNew`

```wolang
offer fun BuilderNew() -> Builder uses unsafe
```

### function `TextFromRaw`

```wolang
offer fun TextFromRaw(data: *u8, length: int) -> text uses unsafe
```

### function `EndsWith`

```wolang
offer fun EndsWith(value: text, suffix: text) -> bool uses unsafe
```

Whether `value` ends with `suffix`.

### function `Contains`

```wolang
offer fun Contains(value: text, needle: text) -> bool uses unsafe
```

Whether `needle` is anywhere in `value`.

### function `LastIndexOf`

```wolang
offer fun LastIndexOf(value: text, needle: text) -> int uses unsafe
```

Where the last `needle` is, or -1.

### function `Substring`

```wolang
offer fun Substring(value: text, from: int, to: int) -> text uses unsafe
```

The bytes from `from` up to `to`, as a view: nothing is copied, and
the result lasts as long as `value` does.

Out-of-range bounds are clamped rather than trapped: a slice of a
string is almost always computed from a search, and a search that
found nothing should give an empty string rather than stop the
program.

### function `After`

```wolang
offer fun After(value: text, needle: text) -> text uses unsafe
```

Everything after the first `needle`, or "" when there is none.

### function `Before`

```wolang
offer fun Before(value: text, needle: text) -> text uses unsafe
```

Everything before the first `needle`, or the whole string.

### function `Trim`

```wolang
offer fun Trim(value: text) -> text uses unsafe
```

`value` without the spaces at either end, as a view.

### function `TrimStart`

```wolang
offer fun TrimStart(value: text) -> text uses unsafe
```

### function `TrimEnd`

```wolang
offer fun TrimEnd(value: text) -> text uses unsafe
```

### class `Split`

```wolang
offer class Split
```

The pieces of a string, between one separator and the next.

A walk rather than a list: each piece is a view into the string, so
`for piece in Pieces(line, ",")` costs one allocation less than
nothing -- and the protocol is the one every type that says how to be
walked uses (D96).

#### field `value`

```wolang
var value: text
```

#### field `separator`

```wolang
var separator: text
```

#### field `at`

```wolang
var at: int = 0
```

#### field `piece`

```wolang
var piece: text = ""
```

#### field `done`

```wolang
var done: bool = false
```

#### method `Iterate`

```wolang
fun Iterate() -> Split
```

#### method `Next`

```wolang
fun Next() -> bool uses unsafe
```

#### method `Value`

```wolang
fun Value() -> text
```

### function `Pieces`

```wolang
offer fun Pieces(value: text, separator: text) -> Split
```

The pieces of `value` separated by `separator`.

An empty separator gives the whole string as one piece, and a string
with nothing in it gives one empty piece -- which is what every
splitter does and what a caller counting fields expects.

### function `CountPieces`

```wolang
offer fun CountPieces(value: text, separator: text) -> int uses unsafe
```

How many pieces there are.

### function `PieceAt`

```wolang
offer fun PieceAt(value: text, separator: text, which: int) -> text
    uses unsafe
```

The `which`th piece, counting from zero, or "".

### function `Join`

```wolang
offer fun Join(pieces: Split, separator: text) -> text uses unsafe
```

The pieces of `value` joined with `separator` between them.

Allocates: the answer is a new string, because the pieces are not next
to each other in memory.

### function `Replace`

```wolang
offer fun Replace(value: text, needle: text, with: text) -> text
    uses unsafe
```

`value` with every `needle` replaced by `with`. Allocates.

### function `Upper`

```wolang
offer fun Upper(value: text) -> text uses unsafe
```

`value` in upper case, for the ASCII letters. Allocates.

### function `Lower`

```wolang
offer fun Lower(value: text) -> text uses unsafe
```

`value` in lower case, for the ASCII letters. Allocates.

### function `Repeat`

```wolang
offer fun Repeat(value: text, times: int) -> text uses unsafe
```

`value` repeated `times` over. Allocates.

### function `PadStart`

```wolang
offer fun PadStart(value: text, width: int, pad: text) -> text uses unsafe
```

`value` with `pad` added on the left until it is `width` long.

### function `PadEnd`

```wolang
offer fun PadEnd(value: text, width: int, pad: text) -> text uses unsafe
```

### function `ParseInt`

```wolang
offer fun ParseInt(value: text) -> int? uses unsafe
```

The whole number `value` spells, or nothing when it does not spell
one.

An optional rather than a fallback, because "this is not a number" and
"this is zero" are different answers and a program reading a
configuration file has to tell them apart.

### function `ParseFloat`

```wolang
offer fun ParseFloat(value: text) -> float? uses unsafe
```

The number `value` spells, or nothing.

## std.thread

### class `Thread`

```wolang
offer class Thread
```

One running thread.

#### field `handle`

```wolang
var handle: u64 = 0
```

#### method `IsRunning`

```wolang
fun IsRunning() -> bool
```

#### method `Join`

```wolang
fun Join() -> bool uses unsafe
```

Waits for it to finish. False when there was nothing to wait for.

### function `Spawn`

```wolang
offer fun Spawn(body: fun()) -> Thread uses unsafe
```

Runs `body` alongside the caller.

The body takes nothing and gives nothing back: what a thread works on
is what it can reach, which in this language means a module-level
`var` or something behind a lock. A parameter would be a pointer whose
lifetime nobody checks.

### function `Parallelism`

```wolang
offer fun Parallelism() -> int uses unsafe
```

How many threads this machine will really run at once.

### class `Mutex`

```wolang
offer class Mutex
```

A lock. `deinit` gives it back, so one that goes out of scope leaves
nothing behind.

#### field `handle`

```wolang
var handle: u64 = 0
```

#### method `deinit`

```wolang
deinit
```

#### method `Lock`

```wolang
fun Lock() uses unsafe
```

#### method `Unlock`

```wolang
fun Unlock() uses unsafe
```

### function `MutexNew`

```wolang
offer fun MutexNew() -> Mutex uses unsafe
```

### function `AtomicAdd`

```wolang
offer fun AtomicAdd(counter: *mut s64, delta: s64) -> s64 uses unsafe
```

Adds `delta` to the number at `counter` and gives back what was there
before, with no other thread able to see a half-done sum.

## std.time

### struct `Instant`

```wolang
offer struct Instant
```

A point in time, as the two numbers every system reports it as.

#### field `seconds`

```wolang
var seconds: s64 = 0
```

#### field `nanos`

```wolang
var nanos: s64 = 0
```

### function `Now`

```wolang
offer fun Now() -> Instant uses unsafe
```

How long the machine has been up, as seconds and nanoseconds.

Monotonic: the difference between two of these is a duration, and it
is never negative no matter what happens to the system clock.

### function `WallClock`

```wolang
offer fun WallClock() -> Instant uses unsafe
```

What the system thinks the time is, as seconds since the epoch.

### function `Elapsed`

```wolang
offer fun Elapsed(from: Instant, to: Instant) -> s64
```

How many nanoseconds there are between `from` and `to`.

### function `ElapsedMillis`

```wolang
offer fun ElapsedMillis(from: Instant, to: Instant) -> s64
```

The same, in milliseconds, which is what a frame budget is written in.
