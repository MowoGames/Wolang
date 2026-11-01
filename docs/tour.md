# A tour of Wolang

Every program here compiles — CI checks them on each commit
([D183](decisions.md)). Read it in order and you will have seen the
whole language in half an hour.

```sh
wolang new tour && cd tour   # a package that runs, tests and builds
wolang run .
```

## Hello

```wolang
need std.io.{Print}

fun main() uses io {
  const name = "world"
  Print("hello, {name}")
}
```

Three things are already visible. `need` brings names in, one module at a
time. `uses io` is the function saying what it may do — printing is an
effect, and a function that prints has to admit it. And `{name}` in a
string is interpolation.

## Values and types

```wolang
need std.io.{Print}

fun main() uses io {
  const fixed = 42            // an int (64 bits, D238), worked out from the value
  var counter = 0             // `var` may change, `const` may not
  counter = counter + fixed

  const pi: float = 3.14159   // or say the type
  const yes: bool = true
  const letter: char = 'w'
  const line: text = "a string is bytes and a length"

  // Sized numbers when they matter, which is most of the time in a
  // file format or a protocol.
  const small: u8 = 255
  const wide: s64 = -9000000000

  Print("{counter} {pi} {yes} {letter} {line} {small} {wide}")
}
```

There are no implicit conversions. `small + 1` is a `u8` and overflows
at 256 — and overflow **traps** rather than wrapping. `std.math` has
`WrappingAdd` for when wrapping is what you meant.

## Functions

```wolang
need std.io.{Print}

fun Add(a: int, b: int) -> int {
  return a + b
}

// A body that is one expression can be written with `=>`.
fun Double(x: int) -> int => x * 2

// Nothing to give back, nothing to declare.
fun Shout(message: text) uses io {
  Print(message)
}

fun main() uses io {
  Shout("{Add(2, 3)} and {Double(20)}")
}
```

## Optionals and failures

Two postfix markers, and a type reads left to right.

```wolang
need std.io.{Print}
need std.text.{ParseInt}

// `T?` may be absent.
fun Half(x: int) -> int? {
  if x % 2 != 0 {
    return null
  }
  return x / 2
}

error TooBig
error Negative

// `T!` may fail. Naming the errors, `T!(A, B)`, lets the compiler check that a
// `match` covers each one.
fun Root(x: int) -> int!(Negative, TooBig) {
  if x < 0 {
    return Negative
  }
  if x > 1000000 {
    return TooBig
  }
  var guess = 0
  while guess * guess < x {
    guess = guess + 1
  }
  return guess
}

fun main() uses io, unsafe {
  // `if const` binds when there is something there.
  if const held = Half(10) {
    Print("half is {held}")
  }
  // `??` is the fallback.
  Print("{Half(7) ?? -1}")

  match Root(144) {
    Ok(value) => { Print("root {value}") },
    Negative => { Print("negative") },
    TooBig => { Print("too big") },
  }

  // `?` passes a failure up to the caller.
  Print("{ParseInt("17") ?? 0}")
}
```

## Branching

```wolang
need std.io.{Print}

enum Shape {
  Dot,
  Circle(radius: float),
  Box(w: float, h: float),
}

fun Area(shape: Shape) -> float {
  // Exhaustive: leave one out and the compiler says which.
  return match shape {
    Shape.Dot => 0.0,
    Shape.Circle(radius) => 3.14159 * radius * radius,
    Shape.Box(w, h) => w * h,
  }
}

fun main() uses io {
  Print("{Area(Shape.Circle(2.0))}")
  Print("{Area(Shape.Box(3.0, 4.0))}")

  const n = 7
  if n > 10 {
    Print("big")
  } else if n > 5 {
    Print("middling")
  } else {
    Print("small")
  }
}
```

## Loops

```wolang
need std.io.{Print}
need std.list.{ListNew}

fun main() uses io, unsafe {
  var total = 0
  for i in 0..5 {           // 0, 1, 2, 3, 4
    total = total + i
  }

  var countdown = 3
  while countdown > 0 {
    countdown = countdown - 1
  }

  var found = 0
  loop {                     // forever, until something leaves
    found = found + 1
    if found == 10 {
      break
    }
  }

  var list = ListNew<int>()
  list.Push(4)
  list.Push(5)
  for value in list {
    total = total + value
  }

  // Labels, for leaving the outer one.
  outer: for i in 0..3 {
    for j in 0..3 {
      if i * j > 2 {
        break outer
      }
    }
  }
  Print("{total} {found}")
}
```

## Structs and methods

```wolang
need std.io.{Print}
need std.math.{Sqrt}

struct Point {
  var x: float = 0.0
  var y: float = 0.0

  // A method takes `self` implicitly, and a field is reachable by name.
  fun Length() -> float uses unsafe {
    return Sqrt(x * x + y * y)
  }

  fun Plus(other: Point) -> Point {
    return Point(x + other.x, y + other.y)
  }
}

fun main() uses io, unsafe {
  const a = Point(3.0, 4.0)
  const b = Point(1.0, 1.0)
  Print("{a.Length()}")        // 5
  Print("{a.Plus(b).x}")       // 4
}
```

A `struct` is a value: assigning one copies it. A `class` is a handle,
and several names can refer to the same object.

## Abilities

An interface, and a type says it has one with `extend`.

```wolang
need std.io.{Print}

ability Greet {
  fun Hello() -> text
}

struct Wolf {
  var name: text
}

struct Robot {
  var serial: int
}

// `extend T as Ability` says a type has one; `extend T` on its own adds
// methods of its own.
extend Wolf as Greet {
  fun Hello() -> text => "awoo, {name}"
}

extend Robot as Greet {
  fun Hello() -> text => "unit {serial} reporting"
}

// Static dispatch: one copy per type, no indirection.
fun Announce<T: Greet>(who: T) -> text {
  return who.Hello()
}

// Dynamic dispatch: one function, whatever comes.
fun AnnounceAny(who: Greet) -> text {
  return who.Hello()
}

fun main() uses io {
  Print(Announce(Wolf("mowo")))
  Print(AnnounceAny(Robot(7)))
}
```

An operator is an ability too. `a + b` on a type that says it has `Add` is
the call `a.Plus(b)`, and `Self` in the ability is the type that has it:

```wolang
need std.io.{Print}
need std.ops.{Add, Eq, Neg, Ord}

struct Vec3 {
  var x: float
  var y: float
  var z: float
}

extend Vec3 as Add {
  fun Plus(other: Self) -> Self => Vec3(x + other.x, y + other.y, z + other.z)
}

extend Vec3 as Neg {
  fun Negate() -> Self => Vec3(-x, -y, -z)
}

extend Vec3 as Eq {
  fun Equals(other: &Self) -> bool => x == other.x && y == other.y && z == other.z
}

// Numbers have every one of these already, so `Twice(2)` works too.
fun Twice<T: Add>(value: T) -> T => value + value

fun main() uses io {
  var v = Vec3(1.0, 2.0, 3.0) + Vec3(0.5, 0.5, 0.5)
  v += v
  const same = -v != v
  Print("{v.x} {Twice(21)} {same}")
}
```

An ability can leave a type for each implementation to choose: `type Item`
is answered with `type Item = int` in the `extend`, and generic code reads
it as `T.Item`. This is how `for` works over anything — `std.iter` says
what being walked means:

```wolang
need std.io.{Print}
need std.iter.{Iterable, Iterator}
need std.list.{ListNew}

class Countdown {
  var from: int = 0
}

class CountdownWalk {
  var at: int = 0
}

extend CountdownWalk as Iterator {
  type Item = int
  fun Next() -> bool {
    at = at - 1
    return at >= 0
  }
  fun Value() -> int => at
}

extend Countdown as Iterable {
  type Walker = CountdownWalk
  fun Iterate() -> CountdownWalk => CountdownWalk(from + 1)
}

// Works for a `List`, a `Countdown` or anything else that can be walked.
fun Last<C: Iterable>(items: C) -> C.Walker.Item? {
  var last: C.Walker.Item? = null
  for value in items {
    last = value
  }
  return last
}

fun main() uses io, unsafe {
  var names = ListNew<text>()
  names.Push("wolang")
  Print("{Last(Countdown(3)) ?? -1} {Last(names) ?? "none"}")
}
```

## Generics

```wolang
need std.io.{Print}
need std.list.{ListNew}

fun Largest<T>(items: []T, before: fun(T, T) -> bool) -> T {
  var best = items[0]
  var i = 1
  while i < items.len {
    if before(best, items[i]) {
      best = items[i]
    }
    i = i + 1
  }
  return best
}

fun main() uses io, unsafe {
  var list = ListNew<int>()
  list.Push(3)
  list.Push(9)
  list.Push(4)
  Print("{Largest(list.Items(), |a, b| a < b)}")   // 9
}
```

Generics are monomorphised: one specialisation per instantiation, no
boxing.

## Closures

```wolang
need std.io.{Print}
need std.list.{Count, ListNew}

fun main() uses io, unsafe {
  var list = ListNew<int>()
  var i = 1
  while i <= 10 {
    list.Push(i)
    i = i + 1
  }
  // The closure takes `limit` with it, by copy.
  const limit = 5
  Print("{Count(list.Items(), |x| x > limit)}")    // 5
}
```

## Effects

A signature says what a function may do, and the compiler checks the
whole call graph.

```wolang
need std.io.{Print}
need std.random.{RandomNew}

// Pure: no effects declared, so it may not print, allocate or ask the
// clock.
fun Pure(a: int, b: int) -> int => a * b + 1

// This one says what it does.
fun Noisy(seed: u64) -> int uses io, unsafe {
  var rng = RandomNew(seed)
  const held = rng.Between(1, 6)
  Print("rolled {held}")
  return held
}

fun main() uses io, unsafe {
  Print("{Pure(6, 7)}")
  const _held = Noisy(42)
}
```

The effects: `io`, `alloc`, `rand`, `time`, `net`, `gpu`, `unsafe`,
`panic`. A function calling something with an effect must declare it or
have it, which is what makes "this runs in the render loop and allocates
nothing" a thing the compiler can check.

## Units and dimensions

```wolang
need std.io.{Print}

dimension Length
dimension Time

unit m: Length
unit s: Time

fun main() uses io {
  const distance = 100m
  const taken = 9.58s
  // The type of this is Length/Time -- a velocity -- and the compiler
  // knows it.
  const speed = distance / taken
  Print("{speed}")
}
```

Adding a `Length` to a `Time` does not compile. It costs nothing at run
time: the units are gone by then.

## Components and systems

```wolang
need std.io.{Print}

component Position {
  var x: float = 0.0
  var y: float = 0.0
}

component Velocity {
  var x: float = 0.0
  var y: float = 0.0
}

tag Falling

// What a system touches is in its signature: `mut` is a write, a bare
// component is a read, and `with` is a filter.
system ApplyGravity(dt: float, q: Query<mut Velocity, with Falling>) {
  foreach v in q {
    v.y = v.y - 9.81 * dt
  }
}

// And when ordering matters, say so.
@After(ApplyGravity)
system Integrate(dt: float, q: Query<mut Position, Velocity>) {
  foreach p in q {
    p.x = p.x + dt
  }
}

fun main() uses io {
  Print("the schedule is worked out at compile time")
}
```

Because the signature says what each system reads and writes, the
compiler builds the frame's schedule and knows which systems may run at
the same time. `wolang systems .` prints it.

## Machines

```wolang
need std.io.{Print}

machine Lamp {
  state Off {
    on Flip -> Warming
  }
  state Warming {
    always -> On
  }
  state On {
    on Flip -> Off
  }
}

fun main() uses io {
  var lamp = Lamp()
  const _fired = lamp.Flip()
  Print("{lamp.IsOn()}")      // true, through Warming
}
```

An unreachable state is warning `W0109`.

## Memory

```wolang
need std.io.{Print}
need std.mem.{ArrayNew, Free}

fun main() uses io, unsafe {
  // A view: a pointer and a length, with bounds checks.
  var room = ArrayNew<int>(4)
  room[0] = 7
  Print("{room[0]} of {room.len}")
  Free(room)

  // A fixed array lives in the frame.
  var fixed: [3]int
  fixed[2] = 9
  Print("{fixed[2]}")

  // An arena: everything taken from it goes back at the end of the
  // block, which is what a frame loop wants.
  arena {
    var held = ArrayNew<int>(1024)
    held[0] = 1
    Print("{held[0]}")
  }
}
```

A `struct` is a value; `Owned<T>` frees what it holds when it goes out
of scope. A `class` is counted: every field, variable and global that
holds one is a reference, and the last one to let go frees it -- with
everything it holds. A cycle is the one thing counting cannot see, and
`weak` is what breaks it:

```wolang
need std.io.{Print}

class Parent {
  var child: Child? = null
}

class Child {
  // Does not keep the parent alive. Reading it gives `null` once the
  // parent is gone.
  weak var parent: Parent? = null
}

fun main() uses io {
  var parent = Parent(null)
  const child = Child(parent)
  parent.child = child
  if child.parent != null {
    Print("still here")
  }
}
```

## Threads

```wolang
need std.io.{Print}
need std.thread.{AtomicAdd, ParallelFor, Spawn}

var total: s64 = 0

fun Work() uses unsafe {
  var i = 0
  while i < 1000 {
    const _held = AtomicAdd(&mut total, 1)
    i = i + 1
  }
}

fun main() uses io, unsafe {
  var worker = Spawn(Work)
  Work()
  const _joined = worker.Join()
  Print("{total}")

  // A range split across the cores: the body is given a span, so a
  // chunk is one call rather than one call per element.
  ParallelFor(1000, 0, |from, to| {
    var at = from
    while at < to {
      const _held = AtomicAdd(&mut total, 1)
      at = at + 1
    }
  })
  Print("{total}")
}
```

## Talking to C

```wolang
need std.io.{Print}
need std.text.{Bytes}

extern "C" fun strlen(s: *u8) -> u64
extern "C" fun getenv(name: *u8) -> *mut u8

fun main() uses io, unsafe {
  const name = Bytes("PATH\0")
  const held = getenv(&name[0])
  if held != ptrnull {
    Print("PATH is {strlen(held)} bytes")
  }
}
```

`wolang bind header.h` writes the declarations for you, and a package's
manifest says which library to link.

## Testing

```wolang
need std.io.{Print}

fun Add(a: int, b: int) -> int => a + b

@Test
fun AddsTwoNumbers() -> bool {
  return Add(2, 2) == 4 && Add(-1, 1) == 0
}

fun main() uses io {
  Print("`wolang test .` runs every @Test, in two tiers, and compares")
}
```

## What to read next

- [The standard library](stdlib.md), module by module, with examples.
- [The cookbook](cookbook.md): a server, a client, configuration, a
  store, a window.
- [The specification](spec.md) for the exact rules.
- [The decisions](decisions.md) for why any of it is the way it is.
