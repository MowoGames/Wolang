# The Wolang language

Status per section: **normative** sections describe what the compiler
implements today; **draft** sections are decided in principle and will be
tightened when the wave that implements them lands.

---

## 1. Lexical structure — normative

### 1.1 Source files

A source file is a sequence of Unicode code points encoded as UTF-8. Malformed
UTF-8 is an error (`E0006`). A leading byte order mark is ignored. A file may
begin with `#!`, in which case the first line is ignored, so a `.wo` file can be
made executable.

The file extension is `.wo`.

### 1.2 Line structure

Statements are separated by line breaks; semicolons are not required. The lexer
does not emit newline tokens. Instead every token records whether it is the
first token on its line, and whether a blank line precedes it, which is what
the parser and the formatter read.

### 1.3 Comments

```wolang
// a comment, ignored
/// documentation, attached to the declaration that follows
//// four or more slashes is an ordinary comment again
```

There are no block comments. Documentation comments are tokens, because they
belong to the declaration they precede; ordinary comments are trivia, recorded
separately for the formatter.

### 1.4 Identifiers

An identifier starts with an ASCII letter, `_`, or a non-ASCII code point, and
continues with those plus ASCII digits. `_` on its own is not an identifier but
a pattern wildcard.

> Non-ASCII identifiers currently accept any well-formed UTF-8 code point.
> Restricting them to the Unicode XID property is deferred; the rule can only
> get stricter, never looser.

### 1.5 Keywords

```
after    as       async    await    before   break    class    component
comptime continue defer    else     enum     extern   false    fn
for      foreach  if       impl     import   in       let      loop
match    mut      null     on       override priv     pub      region
return   self     signal   spawn    struct   system   tag      trait
true     type     unsafe   uses     var      virtual  where    while
```

**Contextual keywords** are lexed as identifiers and only recognized by the
parser where they introduce a declaration:

```
always   derived  dimension  effect  enter  exit  machine  state  unit
```

This is deliberate. Gameplay code names variables `state`, `unit` and `machine`
constantly, and stealing those spellings would be a daily cost for a one-time
grammar convenience.

**Reserved words** may not be used as identifiers (`E0011`), so that adding
them later cannot break existing code:

```
abstract actor  case   catch   const  delete do      final  goto
interface is    kernel lazy    macro  module namespace new   nil
operator package private protected public rollback shader sizeof
static  super  switch template this  throw  transactional try
typedef typeof union  using   void   yield
```

### 1.6 Literals

**Integers** are decimal, or prefixed `0x`, `0o`, `0b` (either case). `_` may
separate digits anywhere after the first one.

```wolang
0  42  1_000_000  0xFF  0o17  0b1010
```

**Floats** require a digit on both sides of the point, which keeps `0..10` a
range and `1.max()` a method call. An exponent may follow.

```wolang
3.14  1e3  1.5e-3  2E+2
```

**Type suffixes** are `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64`. An `f32` or
`f64` suffix makes an otherwise integral literal a float. An integer suffix on
a floating point literal is an error (`E0010`).

```wolang
255u8  -1i64  2.0f64  1f32
```

**Unit suffixes** are any other identifier directly after the digits. Units are
user declared, so the lexer only records the spelling and sema resolves it
against the units in scope. A unit suffix requires a decimal literal.

```wolang
0.5s  200ms  90deg  10m  1.5rad
```

**Strings** are double quoted, may not span lines, and support interpolation
with `{expr}`. Escape sequences are `\n \r \t \0 \\ \" \' \{ \} \u{...}`, where
the Unicode escape takes one to six hexadecimal digits naming a scalar value.

```wolang
"hp {health} / {max}"
"a\tb\u{1F600}"
```

**Raw strings** are prefixed `r`, process no escapes and no interpolation, and
may span lines.

```wolang
r"C:\path\to{file}"
```

**Characters** are single quoted and hold exactly one code point, written
directly or as an escape.

```wolang
'a'  '\n'  'é'
```

### 1.7 Operators and punctuation

```
( ) { } [ ] , ; : . .. ..= -> => @ _
+ += - -= * *= / /= % %=
= == ! != < <= > >= << <<= >> >>=
& &= && | |= || ^ ^= ~
? ?. ?? ??=
```

The lexer always takes the longest match. `<` and `>` are lexed as comparisons;
whether they delimit generic arguments is the parser's decision.

---

## 2. Declarations — draft

```wolang
pub fn Damage(base: float, mult: float = 1.0) -> float {
  return base * mult
}

fn Lerp(a: float, b: float, t: float) -> float => a + (b - a) * t

struct HitInfo {
  var point: Vec3
  var normal: Vec3
  var damage: float = 0.0
}

class Weapon : Object {
  var ammo: int = 30
  fn Reload() { ammo = 30 }
}

enum AiState {
  Idle
  Patrol(route: PatrolRoute)
  Chase(target: Pawn)
}

trait Damageable {
  fn TakeDamage(amount: float, from: Actor?)
}

impl Damageable for Barrel {
  fn TakeDamage(amount: float, from: Actor?) { Explode() }
}

type Health = int
```

`let` binds immutably, `var` mutably. Visibility is module-private by default,
`pub` across modules, `priv` within one file. Methods are statically dispatched
unless declared `virtual`, and overriding requires `override`.

`match` must be exhaustive. Error handling is `Result<T, E>` with the `?`
operator; there are no exceptions. Invariant violations are faults: they stop
the coroutine or actor rather than unwinding.

## 3. Types — draft

| Category | Types |
|---|---|
| Integers | `int` (= `i32`), `i8 i16 i32 i64`, `u8 u16 u32 u64` |
| Floats | `float` (= `f32`), `f64` |
| Other scalars | `bool`, `char` |
| Strings | `String` owning, `str` borrowed |
| Collections | `Array<T>`, `Map<K, V>`, `Set<T>`, `Span<T>` |
| Optionals | `T?`, non-nullable by default |
| Ownership | `Box<T>`, `Rc<T>`, `Arc<T>`, `Weak<T>` |
| Functions | `fn(int) -> bool`, closures |
| Results | `Result<T, E>`, `Option<T>` |

Flow typing narrows optionals:

```wolang
if let t = target { t.TakeDamage(10) }
let name = target?.display_name ?? "none"
```

## 4. Effects and capabilities — draft

Effects are inferred inside a module and explicit on public signatures.

```wolang
effect Telemetry

pub fn LoadLevel(path: str) -> Result<Level> uses io, alloc
pub fn Report(event: str) uses Telemetry
```

Built-in effects: `alloc`, `io`, `rand`, `time`, `net`, `gpu`, `unsafe`,
`panic`. A `comptime` function has no effects by construction. A deterministic
module forbids `rand` and `time`. A hot path forbids `alloc`. A sandboxed module
is granted a capability set and nothing more.

## 5. Units and dimensions — draft

```wolang
dimension Length
dimension Time

unit m: Length
unit cm = 0.01m
unit s: Time
unit ms = 0.001s

let velocity = 10m / 2s        // Length/Time
rotation = Quat.FromAxis(up, 90deg)
```

Dimensions participate in type checking and are erased before code generation,
so unit-safe code costs nothing at runtime.

## 6. Data layout — draft

```wolang
@layout(soa)
struct Particle {
  var position: Vec3
  var velocity: Vec3
  var life: float
}

foreach p in particles {
  p.position += p.velocity * dt
}
```

The attribute decides the physical layout; `foreach` maps the loop body onto
SIMD lanes when the body allows it. The code that reads and writes fields is
identical either way, so the layout is a measurement, not a rewrite.

## 7. Concurrency — draft

```wolang
async fn OpenDoor() {
  await animation.Play("Open")
  await Wait(2s)
  await All(LightsOn(), MusicFade(1s))
}
```

Coroutines are stackless and scheduled by the host's tick groups or by the
runtime's own scheduler. Concurrency is structured: a task belongs to the scope
that spawned it and is cancelled with it.

## 8. State machines — draft

```wolang
machine Door {
  state Closed { on Interact -> Opening }
  state Opening {
    enter { await animation.Play("Open") }
    always -> Open
  }
  state Open { on Interact after 3s -> Closing }
}
```

Checked for unreachable states and for transitions that can never fire.

## 9. Reactive values — draft

```wolang
class Character {
  var base_damage: float = 10.0
  var buffs: Array<Buff>
  derived var damage: float => base_damage * buffs.Product(|b| b.multiplier)
}
```

A `derived` field recomputes when an input changes rather than on every read,
and never needs manual invalidation.

## 10. Entities and systems — draft

```wolang
component Velocity { var value: Vec3 }
tag Falling

system ApplyGravity(dt: float, q: Query<mut Velocity, with Falling>) {
  foreach v in q { v.value.y -= 9.81 * dt }
}
```

The compiler derives an access graph from the queries, schedules
non-conflicting systems in parallel, and rejects two systems that write the
same component without an ordering.

## 11. Compile-time evaluation — draft

```wolang
comptime let SinTable = Array<float>.Generate(256, |i| Sin(i * Tau / 256))

comptime fn Validate(config: WeaponConfig) {
  assert(config.fire_rate > 0, "fire rate must be positive")
}
```

`comptime` runs on the same IR as the rest of the language and replaces macros,
template metaprogramming and external code generators.
