# What is new

One section per release, newest first. The full list, with the decision
behind each change, is in the [changelog](../CHANGELOG.md); how to move a
program from one release to the next is the last table of each section.

## 1.3.0

1.3.0 finishes what an engine asked of the language in 1.2.0 and brings back
the libraries that 1.2.0 cut. A program can save and restore its objects, wait
on a clock it owns, and be reloaded without losing the objects a host holds.
A host can bound a script's time, deny a mod the file system and the network,
and refuse a binding that was built for a different layout. Five libraries
are new: `font`, `ui`, `audio`, `gpu` and `app`. Very little in this release
breaks a 1.2.0 program; the last table lists what does.

### Save a game

Mark a field `@Save` and `std.save` writes it, and every object it reaches,
by field name. A program that gained or lost a field since the save still
reads it. A shared reference stays shared and a cycle comes back as a cycle
(D266).

```wolang
need std.io.{Print}
need std.mem.{Free}
need std.save.{Corrupt, Load, Save}

class Hero {
  @Save
  var level: int = 1
  @Save
  var name: text = ""
  var scratch: int = 0
}

fun main() uses io, unsafe {
  const hero = Hero()
  hero.level = 7
  hero.name = "Ada"
  hero.scratch = 99
  const bytes = Save(hero)
  const back = match Load<Hero>(bytes) {
    Ok(found) => found,
    Corrupt => Hero(),
  }
  Free(bytes)
  Print("{back.name} {back.level} {back.scratch}")
}
```

It prints `Ada 7 0`: `scratch` was not marked. The same facility is how a hot
reload keeps an object a host holds when its class changes (below).

### Wait on the caller's clock

A `task fun` can `wait`. The caller advances time with `Tick(dt)`, so pausing is
not ticking and slow motion is a smaller `dt`; the game needs no hook for its own
time (D267). There is no `routine` keyword: `task fun` and `wait` were already
the spelling for code that suspends, and they compile to the same state
machines in every tier.

```wolang
need std.io.{Print}
need std.tasks.{Done, Now, Tick, Wait}

class Door {
  var open: bool = false
}

task fun OpenLater(door: Door) {
  wait Wait(2.0)
  door.open = true
}

fun main() uses io, unsafe {
  const door = Door()
  const handle = spawn OpenLater(door)
  var frames = 0
  while !Done(handle) {
    const _steps = Tick(1.0)
    frames = frames + 1
  }
  Print("open after {frames} frames, {Now()} seconds: {door.open}")
}
```

A host ticks a VM with `wolang_vm_tick`.

### Replication is metadata

`@Replicated` on a field, and `@Server`, `@Client` and `@Multicast` on a method,
say what a host should send where. Wolang sends nothing itself: the attributes
are checked (`E0175`, `E0176`, `E0178`, `E0179`) and reported through the class
reflection of the C API, so a host can route RPCs and replicate fields without
scanning the source (D268).

### What a host can ask for

The C API is at version 1.5 and still additive. A host built against 1.0 keeps
working.

| Version | What a host gets |
|---|---|
| 1.4 | `wolang_vm_set_budget`: a tick budget for one call, so a runaway script stops with an error instead of hanging the game thread (D265). `wolang_vm_expect_abi` and `wolang_vm_expect_struct`: the generated bindings state the layouts they were built for, and a mismatch fails the load and says which side expected what (D271). `wolang_vm_set_remap`: a reload carries the objects the host holds into the new layout instead of refusing (D266). |
| 1.5 | `wolang_vm_tick` and `wolang_vm_tasks_pending` for tasks on the host's clock (D267), and reflection of the replication attributes (D268). |

A mod is never granted `unsafe`, whatever the host's list says (D270): a
mod granted `io` and `net` still cannot reach the calls std marks `unsafe`. A
trap now carries its source place and the script's call stack to the host's
reporter (D269). The execution budget runs in the interpreter and the x64
compiled tier; on arm64 a budgeted VM runs the interpreter.

### Five libraries

Each library splits into a part that is plain computation, tested with no
window and no device, and a part that talks to a device and answers "no"
cleanly when there is none.

- **`font`** reads TrueType fonts, rasterises glyphs with exact coverage, lays
  text out with kerning and wrapping, and packs an atlas (D276).

```wolang
need font.{LayoutText, Measure, SampleFont}
need std.io.{Print}

fun main() uses io, unsafe {
  match SampleFont() {
    Ok(font) => {
      const width = Measure(font, "HELLO", 16.0)
      Print("{width}")
      const layout = LayoutText(font, "HELLO WORLD", 16.0, 60.0)
      Print("{layout.lines}")
    },
    _ => Print("no font"),
  }
}
```

- **`ui`** is an immediate-mode toolkit: label, button, checkbox, slider, text
  field, list box and panel. A frame makes a draw list of plain data, and a
  software renderer draws it into an image for tests (D277).
- **`audio`** mixes voices on buses with pan and clamping, converts sample rates,
  and reads and writes WAV. Ogg files have their headers read; the audio
  inside is not decoded yet (D279).
- **`gpu`** is a backend-neutral device with buffers, textures, programs and
  passes. It has a recording null backend that tests use, and an OpenGL 3.3
  backend over `gfx` (D280).
- **`app`** is the shell around them: a fixed-timestep clock, window, `gpu` and
  sound wired together, config and per-user paths, and a `RunFrames` that runs
  without a window (D281).

The sections of [the standard library guide](stdlib.md) for each package have
a runnable example.

### Tools

- `wolang bind --reflection` registers each class with a liveness token and
  writes the layouts a binding relies on, so a host that loads a binding built for
  another layout is told (D271, D262). `--no-token` leaves the token out.
- `wl bench` prints calls per millisecond and objects per call for each tier;
  [the measured table](benchmarks.md) has the numbers (D273).
- On Linux, `wolang build` can link with a bundled linker and a musl sysroot
  when the machine has no C compiler (D272). It has been written and its
  command checked, but not yet run on Linux.
- A lexer and a parser for Wolang, written in Wolang, live in `tools/selfhost`.
  They match the C++ ones token for token and tree for tree on the repository's
  own files, and a first tool, `fetch`, is written with them. `std.arena` is the
  bump allocator a compiler wants (D283, D285).

### Also fixed

- A class with an owned `Text` field no longer corrupts the heap when its last
  handle goes, and gives the bytes back when the field is replaced (D289).
- The text an interpolation, a `+` or a `+=` builds is given back (D275).
- An annotated local of an ability type keeps the annotation:
  `var backend: Backend = NullNew()` can take any implementer (D275).
- An unknown error name in a `match` arm on a `T!` is `E0166`, not `E0201`. A
  function held in a local and handed to a `@Thread` function is `E0163`
  unless the caller states `uses unsafe` (D275).
- An or-pattern traps in no tier, and comparing two enum values no longer sends
  the whole function to the interpreter.

### Moving a program to 1.3.0

| If your code | Do this |
|---|---|
| Runs on 1.2.0 and matches no row below | Nothing: it keeps working |
| Names a mod's capabilities with `unsafe` | Remove it; it is never granted (D270) |
| Builds a class with `Class()` and no arguments, and reads a class-typed field | Give the field a value first: such a field is still null, and reading it fails. This is an open gap |
| Hands a held function to a `@Thread` function | State `uses unsafe` or make the caller `@Thread` (`E0163`) |
| Is a host that reloads | Register a remap with `wolang_vm_set_remap`, or a reload that changes a held object's class is refused with a note |

## 1.2.0

1.2.0 makes the breaking language decisions together, so that programs move
once. `int` grows to 64 bits, errors can carry data, `unsafe` becomes
something a function declares, slices say whether they can be written,
and a closure can outlive the call that made it. The embedding API grows what
an engine needs, and `math` arrives.

### Errors carry data, and a function states which ones

`T!` still means "may fail with anything". `T!(A, B)` names the errors, and the
compiler then checks that a `match` covers each of them. An error can hold
values, so the caller learns where and why, not only that it failed (D235).

```wolang
need std.io.{Print}

error Empty
error TooShort(needed: int, got: int)

fun Check(length: int) -> int!(Empty, TooShort) {
  if length == 0 {
    return Empty
  }
  if length < 4 {
    return TooShort(4, length)
  }
  return length
}

fun main() uses io {
  match Check(2) {
    Ok(value) => { Print("fine {value}") },
    Empty => { Print("empty") },
    TooShort(needed, got) => { Print("needed {needed}, got {got}") },
  }
}
```

A function you `offer` must state its errors (`E0150`). A `match` on a `T!`
that does not cover them is `E0201`. `json`, `yaml` and `toml` now report
`Malformed(line, column)`; see
[the cookbook](cookbook.md#report-where-a-parse-failed).

### `unsafe fun`: the caller vouches

A function that is unsafe in itself says so. Whoever calls it states
`uses unsafe`; the body need not (D236).

```wolang
need std.io.{Print}

extern "C" fun abs(x: s32) -> s32

unsafe fun Magnitude(x: s32) -> s32 {
  return abs(x)
}

fun main() uses io, unsafe {
  Print("{Magnitude(-4)}")
}
```

A function with no body that writes `uses unsafe` is now `E0153`: write
`unsafe fun` instead.

### Slices say whether they can be written

`[]T` is read-only and `[]mut T` is writable, the way `*T` and `*mut T` already
were. A `[]mut T` goes where a `[]T` is wanted, and not the other way (D251).
Writing through a `[]T` is `E0165`, and `std.text.Bytes` is no longer `unsafe`.

```wolang
need std.io.{Print}
need std.mem.{ArrayNew, Free}

fun Fill(items: []mut int, value: int) {
  var i = 0
  while i < items.len {
    items[i] = value
    i = i + 1
  }
}

fun Sum(items: []int) -> int {
  var total = 0
  for item in items {
    total = total + item
  }
  return total
}

fun main() uses io, unsafe {
  var room = ArrayNew<int>(4)
  Fill(room, 3)
  Print("{Sum(room)}")
  Free(room)
}
```

### A closure can outlive its call

A function parameter may be called and passed down. To store it, write `keep`.
The closure then owns what it captured (D240).

```wolang
need std.io.{Print}

class Counter {
  var n: int = 0

  fun Bump() -> int {
    n = n + 1
    return n
  }
}

class Button {
  var id: int
  var handler: fun() -> int
}

fun NewButton(id: int, handler: keep fun() -> int) -> Button {
  return Button(id, handler)
}

fun main() uses io {
  const counter = Counter(0)
  const button = NewButton(1, || counter.Bump())
  Print("{button.handler()} {button.handler()}")
}
```

Handing a borrowed function to a `keep` parameter's place is `E0161`, and an
escaping closure cannot take a reference, a pointer or a slice with it
(`E0162`). `keep` anywhere but on a parameter's function type is `E0046`.

### Operators are abilities, and `math` uses them

`a + b` on a type that has `Add` is the call `a.Plus(b)`, and `Self` in an
ability is the type that has it (D239). The new `math` package is built on that:
vectors, matrices and quaternions that add, subtract, negate and compare with
the operators you expect (D263).

```wolang
need math.{Vec3, Mat4Translation, QuatAxisAngle}
need std.io.{Print}

fun main() uses io, unsafe {
  const turn = QuatAxisAngle(Vec3(0.0, 0.0, 1.0), 1.5707963)
  const move = Mat4Translation(Vec3(10.0, 0.0, 0.0))
  const point = move.TransformPoint(turn.Rotate(Vec3(1.0, 0.0, 0.0)))
  Print("{point.x} {point.y} {point.z}")
  const sum = Vec3(1.0, 2.0, 3.0) + Vec3(1.0, 1.0, 1.0)
  Print("{sum.Length()}")
}
```

### Also new

- `@Hot` now checks what a body allocates, across modules (`E0155`, D237). The
  `alloc` effect is gone.
- `after` in a `machine` works through a generated `Tick(dt)`, so pausing is not
  calling it and slow motion is a smaller `dt` (D260).
- A module-level `weak var` of an optional class holds its object weakly
  (D253), and `a?.Method()` gives an optional of the method's result.
- Qualified names work in patterns, as the callee of a built-in and as an
  assignment target (D246).
- `E0147` follows a thread body into other modules and into methods (D252).

### Embedding

The C API is at version 1.3 and still additive: a host built against 1.0
keeps working ([the embedding guide](embedding.md) has the whole list).

| Version | What a host gets |
|---|---|
| 1.1 | Functions named by module, `wolang_vm_function_module`; a lone `.wo` file can `need`; `Print` goes through the host's writer; a VM's globals are released when it is destroyed (D259) |
| 1.2 | A VM keeps its own bindings, classes and writers (D257); a failed reload keeps the old program (D255); `wolang_vm_object_new`, `object_set` and the `wolang_vm_class_*` calls (D258) |
| 1.3 | A host class may register a token callback, so a reused address does not read as the object that was there before (D254) |

The binding generator, `wolang bind --reflection`, now binds static functions,
`std::string` results and properties, numeric structs and object properties,
builds `Quat` as `x, y, z, w`, and asks the host whether an object is alive
before it touches it (D262).

### Moving a program to 1.2.0

| If your code | Do this |
|---|---|
| Relied on a 32-bit `int` wrapping, or passes an `int` to a C function | Write `as s32` where an `int` reaches C, or use `s32` where 32 bits are meant (D238) |
| Has an AOT object built with 1.1 | Build it again; 1.1 objects do not link with 1.2 |
| Stored a text hash or an error tag | Compute it again; both changed |
| Writes through a slice parameter | Write `[]mut T` on that parameter (`E0165`) |
| Has a bodiless function with `uses unsafe` | Write `unsafe fun` and drop `unsafe` from `uses` (`E0153`) |
| Names the `alloc` effect, or `capabilities = "alloc"` | Remove it; `@Hot` checks allocation itself |
| Offers a function that returns `T!` | State its errors, `T!(A, B)` (`E0150`) |
| Matches on a `T!` without `_` | Cover each error or end with `_` (`E0201`) |
| Stores a function parameter | Write `keep fun(...)` on it (`E0161`) |
