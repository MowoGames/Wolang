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
/* a block comment, which /* may nest */ so a section containing one
   can still be commented out */
```

Documentation comments are tokens, because they belong to the declaration they
precede; ordinary comments are trivia, recorded separately for the formatter.

### 1.4 Identifiers

An identifier starts with `_` or a code point with the Unicode `XID_Start`
property, and continues with `_` or `XID_Continue`. `_` on its own is not an
identifier but a pattern wildcard.

Letters from any script are identifiers; emoji, symbols and invisible
formatting characters are not (`E0013`). The restriction is not about taste:
an identifier containing a zero-width space would hide one name inside
another, and two names that render identically would be different symbols.

Identifiers must already be in Unicode Normalization Form C (`E0014`). The
compiler rejects a non-NFC name rather than normalizing it, which is what
guarantees one name has one spelling: `й` written as U+0439 is accepted, and
the same letter written as U+0438 U+0306 is not. A combining mark that cannot
compose with what precedes it is already NFC and is accepted, so Devanagari,
Thai and Vietnamese names work.

### 1.5 Keywords

```
ability   after     arena     as        before    break     class     component
comptime  const     continue  defer     else      enum      error     extend
extern    false     for       foreach   fun       if        in        loop
match     mut       need      null      offer     on        override  priv
ptrnull   return    self      signal    spawn     struct    system    tag
task      true      type      unsafe    uses      var       virtual   wait
where     while
```

`fun` declares a function, `need` brings names in, and `offer` sends a name
out of its module. `need` and `offer` are a matched pair: a module needs names
from elsewhere and offers its own.

A `need` also makes the last segment of its path a qualifier for everything
that module offers, with or without a name list: after `need std.json`,
`json.Parse(text)` is the module's function, which is how a program uses two
modules that both offer `Parse`. A local or declaration of the same name
takes precedence; a name the module does not offer is `E0144`, and a bare
name that two needed modules both offer is `E0143` where it is used (D246).

**Contextual keywords** are lexed as identifiers and only recognized by the
parser where they introduce a declaration:

```
always   aos      deinit   derived  dimension  effect  enter  exit
machine  soa      state    unit     weak       with    without
```

This is deliberate. Gameplay code names variables `state`, `unit` and `machine`
constantly, and stealing those spellings would be a daily cost for a one-time
grammar convenience. `with` and `without` are the query filters of
`Query<mut Velocity, with Falling>`, and are only modifiers when a type
follows them.

**Reserved words** may not be used as identifiers (`E0011`), so that adding
them later cannot break existing code:

```
abstract      actor         async         await         case
catch         delete        do            final         fn
goto          impl          import        interface     is
kernel        lazy          let           macro         module
namespace     new           nil           operator      package
private       protected     pub           public        region
rollback      shader        sizeof        static        super
switch        template      this          throw         transactional
try           typedef       typeof        union         using
void          yield
```

`fn`, `import` and `impl` are reserved because they were the earlier
spellings of `fun`, `need` and `extend`; so are `let`, `async`, `await` and
`region`, which this language used to spell that way and most others still
do. Writing one gets a message instead of a confusing parse failure.

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

**Type suffixes** are `s8 s16 s32 s64 u8 u16 u32 u64 f32 f64`. An `f32` or
`f64` suffix makes an otherwise integral literal a float. An integer suffix on
a floating point literal is an error (`E0010`).

```wolang
255u8  -1s64  2.0f64  1f32
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

## 2. Grammar and parsing — normative

### 2.1 Statement separation

A line break ends a statement. A semicolon is accepted as an explicit
separator, and two statements on one line without one are an error (`E0040`)
rather than a silent join.

An expression continues onto the next line only when the operator stays on the
previous line. An operator that *starts* a line begins a new statement, so this
is two statements and not a subtraction:

```wolang
a
-b
```

Member access is the exception: `.` and `?.` may open a line, because
multi-line method chains are a normal way to write a pipeline.

```wolang
const closest = World.Overlap<Pawn>(position, range)
    .Filter(|p| p.alive)
    .Closest(position)
```

### 2.2 Operator precedence

Loosest first. Every level is left associative except assignment, which is
right associative.

| Level | Operators |
|---|---|
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` `??=` |
| 2 | `..` `..=` |
| 3 | `??` |
| 4 | `\|\|` |
| 5 | `&&` |
| 6 | `==` `!=` `<` `<=` `>` `>=` |
| 7 | `\|` |
| 8 | `^` |
| 9 | `&` |
| 10 | `<<` `>>` |
| 11 | `+` `-` |
| 12 | `*` `/` `%` |
| 13 | `as` |

`+ - * / == != < <= > >=` and unary `-` on a type of your own are calls to an
`@Operator` ability (section 3, D239); the precedence is the same.

Prefix operators (`-`, `!`, `~`, `*`, `&`, `&mut`, `wait`, `spawn`) bind
tighter than any binary operator. Postfix forms — call, index, `.name`,
`?.name`, `?`, and an initializer block — bind tightest of all.

### 2.3 Ranges and loop labels

A range may omit either bound, which is what makes slicing readable:

```wolang
xs[2..]    xs[..5]    xs[..]    xs[1..=3]
```

In the subject of a loop or a condition, `{` opens the body, so `for i in 2..`
followed by a block is a range with no upper bound and not a range ending in a
block.

A loop may be labelled, and `break` and `continue` may name it. A label must
be on the same line as the `break`, so a following statement that happens to
start with an identifier is not swallowed. Naming a loop that does not exist
is an error (`E0044`).

```wolang
outer: for row in grid {
  for cell in row[1..] {
    if cell.blocked { break outer }
    continue outer
  }
}
```

### 2.4 Casts

`as` performs an explicit conversion and takes a type on its right:

```wolang
const index = position as s64
```

### 2.5 Two deliberate ambiguities

**Generic arguments versus comparison.** There is no turbofish. On `<` after a
value, the parser scans ahead: if the angle brackets balance and a `(`
immediately follows the closing `>`, it is a generic call; otherwise `<` is a
comparison.

```wolang
World.Spawn<Projectile>(origin) // generic call
if a < b && c > d { }           // two comparisons
```

The scan accepts only tokens that can appear in a type argument, and
deliberately excludes parentheses — so a function type cannot be written as an
explicit generic argument in expression position, and `(a < b) > (c)` is read
as a generic call. Both are rare enough to be worth the simpler rule.

**Brace blocks versus struct literals.** `Path { ... }` is a struct literal,
and a brace block after a call is an initializer block:

```wolang
const weapon = Weapon { ammo = 30 }
World.Spawn<Projectile>(at, dir) { speed = 40.0 }
```

Inside the subject of `if`, `while`, `for`, `foreach` and `match`, a `{` opens
the body instead, so `if ready { Fire() }` needs no parentheses.

### 2.6 Error recovery

Parsing never fails. A construct that cannot be parsed becomes an error node,
one diagnostic is reported, and the parser resynchronizes on the next
declaration, statement or closing brace. A declaration that follows a broken
one still appears in the tree, which is what the language server needs while a
file is half typed.

The parser also stays quiet about a token the lexer already reported, so a
stray `$$$` produces one message rather than one per layer.

## 3. Declarations — normative

```wolang
offer fun Damage(base: float, mult: float = 1.0) -> float {
  return base * mult
}

fun Lerp(a: float, b: float, t: float) -> float => a + (b - a) * t

struct HitInfo {
  var point: Vec3
  var normal: Vec3
  var damage: float = 0.0
}

class Weapon : Object {
  var ammo: int = 30
  fun Reload() { ammo = 30 }
}

enum AiState {
  Idle
  Patrol(route: PatrolRoute)
  Chase(target: Pawn)
}

ability Damageable {
  fun TakeDamage(amount: float, from: Actor?)
}

extend Vec3 {
  fun Flat() -> Vec3 => Vec3(x, 0.0, z)
}

extend Barrel as Damageable {
  fun TakeDamage(amount: float, from: Actor?) { Explode() }
}

type Health = int

error NotFound, Corrupt
```

`extend T` adds inherent members to a type; `extend T as Ability` implements a
ability for it. `extend<T> Box<T>` extends every `Box`, whatever its argument.

An ability may declare an associated type, `type Item` or `type Walker:
Iterator` with a bound, and every implementation answers it with `type Item =
SomeType`; an implementation that does not is `E0402`. Inside the ability the
bare name is the implementation's choice. Generic code names it through a type
parameter, `C.Item`, and `C.Walker.Item` follows one associated type into the
next; once `C` is known it is whatever that implementation said. The target comes first either way, so the declaration reads as a
sentence.

`Self` inside an ability is the type that implements it, and inside `extend T`
or a type's own body it is `T`; anywhere else it is `E0156`. An ability that
takes or returns `Self` cannot be the type of a value (`E0157`), because a pair
of data and table cannot say what `other` is; it can still be a bound.

An ability marked `@Operator` lends its one method to an operator (D239):
`Plus` is `+`, `Minus` `-`, `Times` `*`, `Divide` `/`, `Negate` unary `-`,
`Equals` `==` and `!=`, and `Compare` `<` `<=` `>` `>=`. A malformed one is
`E0160`. `a + b` on a type that has the ability is the call `a.Plus(b)`, and
`a += b` is `a = a.Plus(b)`; `a != b` is `Equals` and then a not, and `a < b`
compares what `Compare` answers (negative, zero, positive) with zero. The
ability is required, not just a method of the same name. Numbers meet every
operator ability, `bool` and `char` meet `Equals` and `Compare`, and `text`
meets `Equals`, so `T: Add` takes `int`, and a number's operator stays the
machine instruction. `std.ops` has `Add`, `Sub`, `Mul`, `Div`, `Neg`, `Eq`,
`Ord` and `Hash`. (`impl` is reserved, so the Rust spelling produces a message rather
than a confusing parse failure.)

`const` binds a name that never changes and `var` one that does. Two words
rather than one word and a modifier, because mutable locals are most of the
locals gameplay code has and `var x` should not cost more to write than the
immutable form. Visibility is module-private by default,
`offer` across modules, `priv` within one file. Methods are statically dispatched
unless declared `virtual`, and overriding requires `override`.

`match` must be exhaustive. Error handling is `T!` with the `?`
operator (§4.3); there are no exceptions. Invariant violations are faults: they stop
the coroutine or actor rather than unwinding.

## 4. Types — normative

| Category | Types |
|---|---|
| Integers | `int` (= `s64`), `s8 s16 s32 s64`, `u8 u16 u32 u64` |
| Floats | `float` (= `f32`), `f64` |
| Other scalars | `bool`, `char` |
| Strings | `Text` owning, `text` borrowed |
| Fixed arrays | `[N]T`, a language construct: no allocation |
| Slices | `[]T`, a borrowed view: no allocation. An array converts to one |
| Collections | `List<T>`, `Map<K, V>`, `Set<T>` — library types in `std.list`, `std.map`, `std.set` |
| Optionals | `T?`; there is no `Option<T>` type. References are non-nullable by default |
| Fallible | `T!`, a value or any error; `T!A` and `T!(A, B)`, a value or one of the errors named |
| Ownership | `Owned<T>`, `Shared<T>` (library classes in `std.own`); a weak reference is `weak var` (D207) |
| Functions | `fun(int) -> bool`, closures |

`[N]T` and `[]T` are part of the language because `core` needs them and
because they allocate nothing, which is what makes them usable on WASM and on
embedded targets. A growable `List<T>` needs an allocator, so it is a library
type from wave 13 onwards.

### 4.1 Integer literals

An integer literal has arbitrary precision until it is used, and then takes
the type the context expects, defaulting to `int` (`s64`, so a literal with
no type fits up to 2^63 - 1; from 2^63 it is `E0109`):

```wolang
const a = 1                 // int
const b: u8 = 200           // the literal takes u8
const big: u64 = 0x8000000000000000 // fits u64, with no suffix
const bad = 0x8000000000000000 // E0109: defaults to int, which it does not fit
comptime const c = 1 << 100 // exact, no overflow
TakeU8(300)                 // E0101: does not fit in u8
```

### 4.2 Optionals

`T?` is spelling for `Option<T>`: one concept, one implementation, and flow
typing that works the same for values and references. The compiler applies a
niche optimization, so `Actor?` is a nullable pointer at runtime while `int?`
carries a tag.

Optionals need no `Some`/`None` vocabulary, because the language already
spells presence and absence: `T?` in a type, `x?` in a pattern, `null` for
absence, `??` for a default.

```wolang
if const t = target { t.TakeDamage(10) }
const name = target?.display_name ?? "none"

match count {
  n? => n
  null => 0
}
```

### 4.3 Errors

`T!` means "a `T`, or any error". `T!A` and `T!(A, B)` state which errors: a
stated set. `?` never needs a conversion between error types, because a set that
is stated must contain what the body passes on, and the compiler checks that
(`E0151`). Both markers are postfix, so a type reads left to right and they
combine:

```wolang
error NotFound, Corrupt
error Malformed(line: int, column: int)

fun Find(id: int) -> Pawn?                // may be absent
fun Lookup(key: text) -> Pawn?!(NotFound) // either

fun Load(path: text) -> Level!(NotFound, Corrupt) uses io {
  const bytes = ReadFile(path)?           // its errors must be in the set
  if bytes.len == 0 { return Corrupt }
  return Parse(bytes)
}

match Load(p) {
  Ok(level) => Use(level)
  NotFound => Default()
  Corrupt => Report()
}
```

A `match` over a stated set must name every error in it, or have a `_` arm
(`E0201`). A `match` over a bare `T!` must have a `_` arm, because the errors it
may hold are not written anywhere. An offered function states its set
(`E0150`): a caller in another module cannot read the body.

An error may carry data: `error Malformed(line: int, column: int)`. It is built
with its arguments, `return Malformed(3, 14)`, and a `match` arm binds them,
`Malformed(line, column) => ..`; a bare `Malformed =>` ignores them. An error
with data goes into a `T!` whose set names it (`E0152` for a bare `T!`), because
the data is kept beside the tag. An error is told apart from another of the same
name by its module: `json.Malformed` and `yaml.Malformed` are different errors.

### 4.3a Numeric conversions

There are no implicit numeric conversions. Widening is as explicit as
narrowing:

```wolang
const small: s32 = 1
const wide: s64 = small          // E0101: expected `s64`, found `s32`
const fine: s64 = small as s64
```

Integer literals are the exception, and the reason this is not tiresome: a
literal has arbitrary precision until it is used, so it takes whatever type
its context expects.

`as` converts between numbers and nothing else. It exists because there are
no implicit conversions, not as a reinterpretation of bits — converting a
`text`, a struct or a `bool` with it is `E0124`, and reading a value's bits
is what `unsafe` is for.

Two conversions are not numeric and are allowed anyway, both inside
`unsafe`: an address as a number (`handle as u64`, for comparing or
hashing one) and a number as an address (`0x1000 as *mut u8`, for a
binding that was given one). The address no object has is spelled
`ptrnull` — it takes whatever pointer type the context wants, and is what
a C function takes to mean *nothing here* and gives back to mean *this
failed*:

```wolang
extern "C" fun getenv(name: *u8) -> *mut u8

fun Lookup(name: []u8) -> bool uses unsafe {
  var found: *mut u8 = ptrnull
  found = getenv(&name[0])
  return found != ptrnull
}
```

A conversion that cannot hold the value **traps**. Overflow traps everywhere
else in this language, so a cast that quietly kept the low bits would be the
one place a number changes silently:

```wolang
fun Narrow(n: s32) -> u8 => n as u8

Narrow(200) // 200
Narrow(300) // traps: the value does not fit
Narrow(-1)  // traps: and neither does this one
```

A float converted to an integer truncates toward zero — `-2.75 as int` is
`-2` — and traps when the truncated value does not fit, or when it is not a
number. A float converted to a narrower float rounds; an integer converted
to a float rounds when the integer has more significant bits than the float
has mantissa, which is not a trap because no information about magnitude is
lost.

### 4.3b Aliases and sizes

`type Count = int` is a second spelling, not a new type: a `Count` is an
`int` everywhere. A chain of aliases settles whatever order they are written
in, and an alias that comes back to itself is `E0118`.

A value type stores its members inline, so it cannot contain itself —
directly, or through an optional, an array or a tuple (`E0117`). One
indirection is enough: a slice, a reference, or a `class`, which is a handle
to a heap object.

```wolang
struct Node { var next: Node }   // E0117: no size
struct List { var rest: []List } // fine: a slice is a view
class Chain { var next: Chain }  // fine: a class is a handle
```

### 4.4 Members

A type's members are its fields, its methods, its enum variants and its
signals. Inside a method a bare name may be a member of the enclosing type,
so `health` means `self.health` -- which is how gameplay code reads.

```wolang
class Pawn {
  var health: float = 100.0

  fun Hurt(amount: float) {
    health = health - amount    // the field, no `self.` needed
  }
}
```

`extend T { ... }` adds members to `T` rather than declaring anything of its
own, so a type's members may come from several files.

A type name used as a callee is a constructor, filling the fields in
declaration order. No arguments asks for the default value:

```wolang
const v = Vec3(1.0, 2.0, 3.0)
const zero = Vec3()
const bad = Vec3(1.0)          // E0102: Vec3 has 3 fields, found 1 argument
```

### 4.5 Generics

Type parameters are inferred from the arguments at a call site, and may
also be written: `Box<int>(7)`, `Get<Position>(world, e)`. A name followed by
`<` is read as a generic call under the rule in section 2.5; there is no
turbofish.

```wolang
fun First<T>(items: []T) -> T => items[0]

const a = First(xs) // T = int,   a: int
const b = First(ys) // T = float, b: float
const c = First(1)  // E0101: expected `[]T`, found `s32`
```

A parameter inferred once cannot be inferred differently by a later argument.
Every instantiation is monomorphized: one specialization is emitted per
distinct set of type arguments.

### 4.6 Exhaustiveness

A `match` has to cover every value its subject can take, because a value that
falls through has nowhere to go. `bool`, optionals and enums have a value set
the compiler enumerates, and `E0201` names what is missing; every other type
needs a catch-all arm. An arm with a guard covers nothing, because the guard
may be false.

```wolang
match shape {
  Circle(r) => Area(r)
  Point => 0.0
}                            // E0201: does not cover `Square`
```

A bare name that is a variant of the subject's enum is the short form of that
variant, not a new binding.

## 5. Effects and capabilities — normative

An effect is something a function does that is not visible in its type:
allocating, touching the filesystem, reading the clock. A function's **effect
row** is the set of effects it may produce.

```wolang
effect Telemetry

offer fun LoadLevel(path: text) -> Level! uses io, alloc
offer fun Report(event: text) uses Telemetry
```

Built-in effects: `alloc`, `io`, `rand`, `time`, `net`, `gpu`, `unsafe`,
`panic`. An `unsafe { }` block *is* the `unsafe` effect, so a caller that
forbids it does not have to look inside. `effect Name` declares one more, and
a declared effect is interned by its declaration — so a project's own
`effect io` is a different effect from the built-in one.

### 5.1 Inference and the module boundary

A row is **inferred inside a module** and **stated on offered signatures**.

Inference is a fixpoint over the whole module at once, so a set of mutually
recursive functions gets one answer rather than one that depends on which
declaration was looked at first:

```wolang
fun Ping() uses io { Pong() }
fun Pong() { Ping(); Emit() }     // no row of its own
fun Emit() uses Telemetry {}
```

Here `Pong` and `Ping` both do `io, Telemetry`, and `Ping` is an error
(`E0701`) because it states only `io`.

A declaration that states nothing is not contradicted — there is nothing to
contradict — but its inferred row still propagates to its callers. The one
exception is a function other modules can call: inference stops at the module
boundary, so an `offer fun` with effects has to say what they are (`E0704`).

### 5.2 Contexts that forbid effects

A context outranks a signature: declaring an effect does not make it allowed.

| Context | Forbids |
|---|---|
| `comptime` | everything but `panic` — it runs inside the compiler |
| `@Deterministic` | `rand`, `time`, `net` |
| `@Hot` | `alloc` |

```wolang
@Deterministic
fun Simulate() uses rand {        // E0702: not allowed here
  Roll()
}
```

This is one mechanism rather than three attribute checks, which is the whole
reason effects exist as a language feature rather than a lint.

### 5.3 Capabilities

A package may be granted a capability set and nothing more:

```toml
[build]
capabilities = "io, alloc"
```

Anything outside the grant is `E0703` — not a mistake in the code, but a
permission the package was not given. Omitting the key leaves a package
unrestricted; `capabilities = ""` sandboxes it completely. `unsafe = "deny"`
is the same idea for one effect, reported as `E0801`.

This is what makes mods safe to run: a mod package is granted what it needs,
and the compiler refuses anything else before a line of it executes. A
sandboxed package may not declare `extern` (`E0705`), and an `extern`
declaration states the capability its primitive needs — `uses io`, `uses
net` — which, unlike `unsafe`, a wrapper that declares it does not absorb.

A wrapper with a body that declares `uses unsafe` absorbs it, but a primitive
with no body (or an `extern`) re-exports `unsafe` to every caller (D221).
Naming a function as a value, not only calling it, charges its whole row to
the enclosing function, comptime included (D222).

## 6. Units and dimensions — normative

```wolang
dimension Length
dimension Time

unit m: Length
unit cm = 0.01m
unit s: Time
unit ms = 0.001s

const velocity = 10m / 2s        // Length/Time
const wrong = 10m + 2s           // E1001: dimensions do not match
```

Multiplying two quantities adds their exponents and dividing subtracts them,
so a product is dimensionless exactly when everything cancels: `10m / 2m` is
a plain number and works anywhere one does.

A **derived** dimension is declared rather than written inline, which keeps
type syntax free of operators and makes the concept nameable:

```wolang
dimension Velocity = Length / Time
dimension Area = Length^2
```

A dimension name **is** a type: the type of quantities carrying it. That is
what makes a signature readable, and what makes swapping two arguments a
compile error rather than a bug:

```wolang
fun TravelTime(distance: Length, speed: Velocity) -> Time {
  return distance / speed
}

TravelTime(3m, 2m / 1s)        // fine
TravelTime(3s, 2m / 1s)        // E0101: expected `Length`, found `Time`
```

A unit suffix on a number is just a symbol to the lexer; units are user
declared, so only name resolution can say what `m` means. A dimension is
interned by its declaration, so `Length` declared once is the same dimension
in every file that needs it.

Dimensions participate in type checking and are erased before code generation,
so unit-safe code costs nothing at runtime.

## 7. Data layout

```wolang
struct Particle {
  var x: float
  var y: float
  var life: float
}

const burst = SoaNew<Particle>(1000) // three parallel arrays
var prototype: Particle              // one value, laid out as written

for i in 0..burst.len {              // the same code either way
  burst[i].x = burst[i].x + dt
}
```

Layout is a property of the **storage**, not of the element type. That is what
lets the same `Particle` be a single value in one place and a struct-of-arrays
in another, and it means the choice can be flipped where it is measured rather
than decided when the type is first written.

Each field of a `soa []T` gets an array of its own, laid out in declaration
order. Reading or writing a field looks the same as on any other storage;
what differs is the traffic. A pass over one field of a sixteen-field record
moves four megabytes per million elements where the record laid out whole
moves sixty-four, and the benchmark in `benchmarks/entities` measures both
directions: one field of a wide record is 1.4x faster as `soa`, and a pass
that touches most of a record is faster laid out whole. The layout is a
measurement, not a rewrite.

Still draft: `foreach` over SIMD lanes, and a function that works on both
layouts. The layout is in the type, so `fun Sum(bodies: soa []Body)` takes the
one it was written for.

## 8. Concurrency — draft

```wolang
task fun OpenDoor() {
  wait animation.Play("Open")
  wait Wait(2s)
  wait All(LightsOn(), MusicFade(1s))
}
```

`task fun` declares a function that can suspend and `wait` is where it does.
Both are shorter than the words most languages use for this, and `wait` says
what the machine is doing rather than naming a protocol.

Suspension is also an **effect**: a `task` signature states `async` whether
or not it has a `uses` clause, so a caller sees it, and a context that
forbids the effect forbids suspending -- `comptime` because there is nothing
to suspend into, `@Hot` because a frame that yields is a frame that missed.
`wait` outside a `task` is `E0301`; calling a task without `wait` or `spawn`
is `E0302`, because calling one starts work rather than doing it.

`spawn` starts a task alongside the caller. Everything the task does carries
over to whoever started it -- a sandboxed package cannot spawn its way to
the network -- except `async`, because the spawner does not wait.

Coroutines are stackless and scheduled by the host's tick groups or by the
runtime's own scheduler. A spawned task runs to its end: nothing cancels
it when the scope that started it exits. Stopping one early is a flag it
reads (D243).

## 9. State machines — draft

```wolang
machine Door {
  state Closed { on Interact -> Opening }
  state Opening {
    enter { wait animation.Play("Open") }
    always -> Open
  }
  state Open { on Interact -> Closing }
}
```

Checked for unreachable states and for transitions that can never fire.
`on Event after 3s -> State` is reserved for a machine that is given time by
a generated `Tick(dt)`; it is `E0159` until then (D243).

## 10. Reactive values — draft

```wolang
class Character {
  var base_damage: float = 10.0
  var buffs: Array<Buff>
  derived var damage: float => base_damage * buffs.Product(|b| b.multiplier)
}
```

A `derived` field recomputes when an input changes rather than on every read,
and never needs manual invalidation.

## 11. Entities and systems — partly draft

```wolang
component Velocity { var x: float = 0.0 }
tag Falling

system ApplyGravity(dt: float, q: Query<mut Velocity, with Falling>) {
  foreach v in q { v.x = v.x - 9.81 * dt }
}

@After(ApplyGravity)
system Integrate(q: Query<mut Position, Velocity>) {
  foreach p in q { p.x = p.x + 1.0 }
}
```

A system declares its access in its signature: `mut T` writes the
component, `T` reads it, and `with T` / `without T` filter on it, which is
reading whether an entity has it. From those declarations the compiler
derives an **access graph** and lays the systems out in stages -- every
system in a stage may run at the same time as the others, because nothing
in it writes what another reads or writes. `wolang systems <dir>` prints
the result:

```
module game
    system ApplyGravity(mut Velocity, Falling)
    system Integrate(mut Position, Velocity)
    stage 1: ApplyGravity
    stage 2: Integrate
```

Two systems that would land in the same stage and touch the same component,
where one writes it, are rejected (`E0127`): the program has to say which
runs first with `@After` or `@Before`. An ordering that names nothing is
`E0128`, and a cycle of orderings is `E0129`.

`mut` in a query is also what makes the loop's binding assignable, so a
system that writes says so once, in its signature, rather than again in
its body.

Systems run. `RunSystems` runs them in stages, and the systems of a stage
run in parallel on threads, because the access graph has proved that none
of them writes what another reads (see "Running the systems" in the
cookbook). Still draft: where the components *live*. The storage is a
library today -- `game.ecs` offers a dense `Store<T>` over `soa []T` and a
`PackedStore<T>` over `[]T`, with the same interface over both layouts.

## 12. Compile-time evaluation — draft

```wolang
comptime const TableSize = 16 * 16

comptime fun Validate(config: WeaponConfig) {
  assert(config.fire_rate > 0, "fire rate must be positive")
}
```

`comptime` runs on the same IR as the rest of the language and replaces macros,
template metaprogramming and external code generators.

## 13. Returning a value

A function that declares a result gives one back with `return`, on every
path that can reach the end of its body:

```wolang
fun Describe(count: int) -> text {
  if count == 0 {
    return "none"
  }
  return "some"
}
```

A function whose whole body is one expression is written with `=>`, and
that expression is what it gives back:

```wolang
fun Add(a: int, b: int) -> int => a + b
```

A block does not give its last expression back. `fun F() -> int { 42 }`
is an error (`E0132`), because the value of a block is not the value of
the function that has it as a body -- what reads as a returned value in
one language would be a silently discarded one here. `if` and `match`
*are* expressions, so `return match step { ... }` is how a function
returns one of several values.

A function that never reaches its end needs no `return`: `loop { }` with
no `break` in it is a body that never finishes.

## 14. Walking a collection

`for value in thing` works on a range, an array, a slice, and on any type
that says how to walk itself:

```wolang
offer class ListWalk<T> {
  var over: List<T>
  var at: int = -1

  fun Next() -> bool {
    at = at + 1
    return at < over.Len()
  }

  fun Value() -> T {
    return over.At(at)
  }
}
```

`Iterate()` hands back something with `Next` and `Value`, and the loop is
those three calls. Walking borrows what it walks, so a collection that
owns its storage is still there afterwards.

## 15. Closures

```wolang
fun Apply(f: fun(int) -> int, x: int) -> int => f(x)

fun Use() -> int {
  const once = Apply(|n| n + 1, 10)
  const typed = Apply(|n: int| -> int { return n * 3 }, 4)
  return once + typed
}
```

A closure is a function written where it is used. Its parameter types come
from the type it is being given to, so a closure passed to something that
takes a `fun(int) -> int` needs no annotation; one written with no such
context has to say them (`E0134`).

A closure takes a **copy** of whatever of the surrounding locals its body
reads:

```wolang
fun Loud(words: []text, limit: int) -> int uses unsafe {
  // `limit` is copied into the closure's environment.
  return Count(words, |word| word.len > limit)
}
```

The copies live in the frame of the call that made the closure, which is
why a closure that captures may be passed *down* but not handed upwards:
returning one, assigning one to a module-level `var` or putting one in a
field is an error (`E0136`). A closure that captures nothing is an
ordinary function value and may go anywhere.

State that outlives a call is what a type with a method is for. That is
not a workaround: an environment with a lifetime of its own is an
allocation someone has to own, and a class is where this language says
who.

A function value is two words: where the code is, and what it closed
over. A plain function closed over nothing, and a call through a value
asks which of the two it has.
