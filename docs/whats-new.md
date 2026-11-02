# What is new

One section per release, newest first. The full list, with the decision
behind each change, is in the [changelog](../CHANGELOG.md); how to move a
program from one release to the next is the last table of each section.

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
