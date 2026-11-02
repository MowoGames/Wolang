# std

## std.compress

### error `Malformed`

```wolang
offer error Malformed
```

What a stream that is not one answers with.

### function `Inflate`

```wolang
offer fun Inflate(bytes: []u8) -> []mut u8!Malformed uses unsafe
```

Reads a deflate stream. There is no cap on the output, so a hostile
stream can expand about a thousandfold; use `InflateLimited` on anything
that came from outside (D227).

### function `InflateLimited`

```wolang
offer fun InflateLimited(bytes: []u8, most: int) -> []mut u8!Malformed uses unsafe
```

Reads a deflate stream, failing with `Malformed` once the output would
pass `most` bytes -- the guard against a decompression bomb (D227).

### function `Deflate`

```wolang
offer fun Deflate(bytes: []u8) -> []mut u8 uses unsafe
```

Compresses with fixed Huffman codes and a window of matches.

One block, fixed codes: a dynamic block saves a few more percent and
costs a Huffman tree builder, which is worth having and is not what
a program reaching for this needs first (D177).

### function `Gzip`

```wolang
offer fun Gzip(bytes: []u8) -> []mut u8 uses unsafe
```

Wraps deflated bytes in a gzip container, which is what a `.gz` is.

### function `Gunzip`

```wolang
offer fun Gunzip(bytes: []u8) -> []mut u8!Malformed uses unsafe
```

Reads a gzip container. Unlimited output; see `GunzipLimited` (D227).

### function `GunzipLimited`

```wolang
offer fun GunzipLimited(bytes: []u8, most: int) -> []mut u8!Malformed uses unsafe
```

Reads a gzip container, failing with `Malformed` once the output would
pass `most` bytes (D227).

### function `Crc32`

```wolang
offer fun Crc32(bytes: []u8) -> u32
```

The CRC-32 a gzip container carries, which is how a reader knows the
bytes arrived whole.

## std.crypto

### function `Sha256`

```wolang
offer fun Sha256(bytes: []u8) -> []u8 uses unsafe
```

SHA-256 of `bytes`, as 32 bytes.

### function `Sha256Of`

```wolang
offer fun Sha256Of(value: text) -> []u8 uses unsafe
```

The same, over text.

### function `Hex`

```wolang
offer fun Hex(digest: []u8) -> text uses unsafe
```

A digest as lowercase hexadecimal, which is how one is written down.

### function `Hmac`

```wolang
offer fun Hmac(key: []u8, message: []u8) -> []u8 uses unsafe
```

HMAC-SHA-256 of `message` with `key`, as 32 bytes.

What signs a cookie, a webhook or a token: a hash of the message and
the key together, arranged so that knowing the hash of one message
does not let someone sign another.

### function `SameDigest`

```wolang
offer fun SameDigest(left: []u8, right: []u8) -> bool
```

Whether two digests are the same, in time that does not depend on
where they differ.

A comparison that stops at the first difference tells an attacker how
much of a signature they got right, which is enough to find the rest
one byte at a time.

### function `RandomBytes`

```wolang
offer fun RandomBytes(count: int) -> []mut u8 uses rand, unsafe
```

`count` bytes from the system's own source.

Not `std.random`: that is a sequence, and a token made from a
sequence is a token someone can continue (D170).

### function `Token`

```wolang
offer fun Token(bytes: int) -> text uses rand, unsafe
```

A token of `bytes` random bytes, as hexadecimal.

### function `Uuid`

```wolang
offer fun Uuid() -> text uses rand, unsafe
```

A version 4 UUID: random, in the shape everything expects.

### function `UuidAt`

```wolang
offer fun UuidAt(millis: s64) -> text uses rand, unsafe
```

A version 7 UUID: the time first, then randomness.

Two of them made a millisecond apart sort in the order they were
made, which is what a database index wants and what version 4 does
not give.

### function `ToBase32`

```wolang
offer fun ToBase32(bytes: []u8) -> text uses unsafe
```

Base32 as RFC 4648 writes it: what a one-time password secret and a
case-insensitive identifier are written in.

### function `FromBase32`

```wolang
offer fun FromBase32(value: text) -> []u8 uses unsafe
```

The bytes a base32 string spells. Anything that is not a digit of the
alphabet is skipped, which is what makes a written-down secret with
spaces in it work.

## std.csv

### function `Rows`

```wolang
offer fun Rows(input: text, separator: u8) -> List<List<text>> uses unsafe
```

Every row of `input`, split on `separator`.

### function `Read`

```wolang
offer fun Read(input: text) -> List<List<text>> uses unsafe
```

The same, with a comma.

### function `Named`

```wolang
offer fun Named(input: text, separator: u8) -> List<Value> uses unsafe
```

Rows as tables, taking the first row as the names.

What a file with a header line is for: `Get(row, "price")` rather
than `row[3]` and a comment saying which column that is.

### function `Write`

```wolang
offer fun Write(rows: &List<List<text>>, separator: u8) -> text uses unsafe
```

Rows as CSV.

A field is quoted when it has to be -- a separator, a quote or a line
break in it -- and not otherwise, which is what keeps a written file
readable.

### function `Field`

```wolang
offer fun Field(value: text, separator: u8) -> text uses unsafe
```

One field, quoted if it has to be.

## std.deque

### class `Deque`

```wolang
offer class Deque<T>
```

#### field `items`

```wolang
var items: []mut T
```

#### field `first`

```wolang
var first: int = 0
```

Where the first element is.

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

#### method `IsEmpty`

```wolang
fun IsEmpty() -> bool
```

#### method `Iterate`

```wolang
fun Iterate() -> DequeWalk<T>
```

What `for x in deque` walks: front to back.

#### method `Slot`

```wolang
fun Slot(index: int) -> int
```

Where element `index` is in the storage.

#### method `At`

```wolang
fun At(index: int) -> T
```

The `index`th from the front.

#### method `Set`

```wolang
fun Set(index: int, value: T)
```

#### method `Push`

```wolang
fun Push(value: T) uses unsafe
```

Adds to the back.

#### method `PushFront`

```wolang
fun PushFront(value: T) uses unsafe
```

Adds to the front.

#### method `Pop`

```wolang
fun Pop() -> T
```

Takes from the front. The caller checks `Len` first: a queue with
nothing in it has nothing to give, and an optional of `T` would be
a second shape to lay out for every `T`.

#### method `PopBack`

```wolang
fun PopBack() -> T
```

Takes from the back.

#### method `Front`

```wolang
fun Front() -> T
```

#### method `Back`

```wolang
fun Back() -> T
```

#### method `Clear`

```wolang
fun Clear()
```

#### method `Reserve`

```wolang
fun Reserve(wanted: int) uses unsafe
```

Makes room for `wanted` elements.

Growing a ring means laying it out straight again: the elements are
copied in order, so `first` is zero afterwards and the arithmetic
above stays the same.

### class `DequeWalk`

```wolang
offer class DequeWalk<T>
```

Walks a deque from front to back.

#### field `over`

```wolang
var over: Deque<T>
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

### function `DequeNew`

```wolang
offer fun DequeNew<T>() -> Deque<T> uses unsafe
```

## std.doc

### enum `Value`

```wolang
offer enum Value
```

One of the six things a document value can be.

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
Array(items: List<Value>)
```

#### method `Object`

```wolang
Object(fields: List<Field>)
```

### struct `Field`

```wolang
offer struct Field
```

A member of a table: its name and its value.

#### field `name`

```wolang
var name: text
```

#### field `value`

```wolang
var value: Value
```

### function `Kind`

```wolang
offer fun Kind(value: Value) -> int
```

Which of the six a value is, as a number: `0` null, `1` bool, `2`
number, `3` text, `4` array, `5` object.

For a program that switches on the shape of a document it did not
write, which is most of what reading one is.

### function `Member`

```wolang
offer fun Member(value: Value, name: text) -> Value? uses unsafe
```

The member of a table called `name`, or nothing.

### function `At`

```wolang
offer fun At(value: Value, at: int) -> Value? uses unsafe
```

Element `at` of a list, or nothing.

### function `Get`

```wolang
offer fun Get(value: Value, path: text) -> Value? uses unsafe
```

What `path` names, or nothing.

`"server.ports.0"`: a name walks into a table and a number walks into
a list. What reading configuration actually looks like, written once
here rather than as four lines at every call.

### function `Len`

```wolang
offer fun Len(value: Value) -> int uses unsafe
```

How many elements a list has, or how many members a table has.

### function `NameAt`

```wolang
offer fun NameAt(value: Value, at: int) -> text uses unsafe
```

The name of member `at` of a table, or "".

### function `ValueAt`

```wolang
offer fun ValueAt(value: Value, at: int) -> Value uses unsafe
```

The value of member `at` of a table.

### function `AsNumber`

```wolang
offer fun AsNumber(value: Value, fallback: float) -> float
```

The number this is, or `fallback`.

### function `AsInt`

```wolang
offer fun AsInt(value: Value, fallback: int) -> int
```

The whole number this is, or `fallback`.

### function `AsText`

```wolang
offer fun AsText(value: Value, fallback: text) -> text
```

The text this is, or `fallback`.

### function `AsBool`

```wolang
offer fun AsBool(value: Value, fallback: bool) -> bool
```

The truth this is, or `fallback`.

### function `IsNull`

```wolang
offer fun IsNull(value: Value) -> bool
```

### function `Object`

```wolang
offer fun Object() -> Value uses unsafe
```

An empty table, to put members in.

### function `Array`

```wolang
offer fun Array() -> Value uses unsafe
```

An empty list, to put values in.

### function `Put`

```wolang
offer fun Put(object: Value, name: text, value: Value) -> Value uses unsafe
```

Puts a member in a table. The table is what it answers with, so calls
chain: `Put(Put(Object(), "a", ..), "b", ..)`.

### function `Push`

```wolang
offer fun Push(array: Value, value: Value) -> Value uses unsafe
```

Adds a value to a list.

### function `Number`

```wolang
offer fun Number(value: float) -> Value =>
```

### function `OfInt`

```wolang
offer fun OfInt(value: int) -> Value =>
```

### function `OfText`

```wolang
offer fun OfText(value: text) -> Value =>
```

### function `OfBool`

```wolang
offer fun OfBool(value: bool) -> Value =>
```

### function `Null`

```wolang
offer fun Null() -> Value =>
```

## std.encoding

### function `ToBase64`

```wolang
offer fun ToBase64(bytes: []u8) -> text uses unsafe
```

`bytes` as base64, padded with `=`.

### function `TextToBase64`

```wolang
offer fun TextToBase64(value: text) -> text uses unsafe
```

A string as base64.

### function `FromBase64`

```wolang
offer fun FromBase64(value: text) -> text? uses unsafe
```

What `value` holds as base64, or nothing when it is not base64.

An optional, because a token that arrived over a wire is exactly the
place where "this is not valid" has to be an answer rather than a
trap.

### function `ToHex`

```wolang
offer fun ToHex(bytes: []u8) -> text uses unsafe
```

`bytes` as lowercase hexadecimal.

### function `FromHex`

```wolang
offer fun FromHex(value: text) -> text? uses unsafe
```

What a hexadecimal string holds, or nothing.

### function `ToUrl`

```wolang
offer fun ToUrl(value: text) -> text uses unsafe
```

`value` with everything a URL cannot carry written as `%XX`.

### function `FromUrl`

```wolang
offer fun FromUrl(value: text) -> text uses unsafe
```

`value` with `%XX` and `+` read back.

## std.env

### function `System`

```wolang
offer fun System() -> text uses io, unsafe
```

Which system this is running on: "linux", "macos" or "windows".

### function `ArgCount`

```wolang
offer fun ArgCount() -> int uses io, unsafe
```

How many arguments the program was given.

### function `ArgAt`

```wolang
offer fun ArgAt(index: int) -> text uses io, unsafe
```

Argument `index`, counting from zero, or "".

### function `Lookup`

```wolang
offer fun Lookup(name: text) -> text? uses io, unsafe
```

The environment variable called `name`, or nothing.

An optional rather than "" because a variable set to nothing and one
that is not there mean different things to a program reading
configuration.

### function `Get`

```wolang
offer fun Get(name: text, fallback: text) -> text uses io, unsafe
```

The environment variable called `name`, or `fallback`.

### function `Set`

```wolang
offer fun Set(name: text, value: text) -> bool uses io, unsafe
```

Sets one, for this program and whatever it starts. True on success.

## std.fmt

### function `Int`

```wolang
offer fun Int(value: int) -> text
```

`value` in base ten.

### function `IntWidth`

```wolang
offer fun IntWidth(value: int, width: int) -> text uses unsafe
```

`value` in base ten, right-aligned in `width` with spaces.

### function `IntZeros`

```wolang
offer fun IntZeros(value: int, width: int) -> text uses unsafe
```

`value` in base ten, right-aligned in `width` with zeros.

The sign stays in front of the zeros, which is what a reader of a
timestamp or an identifier expects.

### function `Radix`

```wolang
offer fun Radix(value: int, base: int) -> text uses unsafe
```

`value` in base `base`, from 2 to 16.

### function `Hex`

```wolang
offer fun Hex(value: int) -> text uses unsafe
```

`value` in hexadecimal, with no prefix.

### function `HexWidth`

```wolang
offer fun HexWidth(value: int, width: int) -> text uses unsafe
```

`value` in hexadecimal, `width` digits wide.

### function `Binary`

```wolang
offer fun Binary(value: int) -> text uses unsafe
```

`value` in binary.

### function `Float`

```wolang
offer fun Float(value: float, decimals: int) -> text uses unsafe
```

`value` with `decimals` digits after the point.

Rounded half away from zero, which is what a person reading a price
expects -- and said here because the other roundings are defensible
too.

### function `Percent`

```wolang
offer fun Percent(value: float, decimals: int) -> text uses unsafe
```

`value` as a percentage with `decimals` digits: `0.256` is `25.6%`.

### function `Bytes`

```wolang
offer fun Bytes(count: int) -> text uses unsafe
```

A count of bytes as a person would read it: `1536` is `1.5 KiB`.

### function `Duration`

```wolang
offer fun Duration(nanos: s64) -> text uses unsafe
```

A duration in nanoseconds as a person would read it.

### function `Left`

```wolang
offer fun Left(value: text, width: int) -> text uses unsafe
```

`value` padded on the left to `width`.

### function `Right`

```wolang
offer fun Right(value: text, width: int) -> text uses unsafe
```

`value` padded on the right to `width`.

### function `Centre`

```wolang
offer fun Centre(value: text, width: int) -> text uses unsafe
```

`value` centred in `width`.

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
fun Read(buffer: []mut u8) -> int uses unsafe
```

Fills `buffer` and says how many bytes arrived: zero at the end of
the file, negative when the system refused.

#### method `Write`

```wolang
fun Write(bytes: []u8) -> int uses unsafe
```

### function `Open`

```wolang
offer fun Open(path: text) -> File uses io, unsafe
```

Opens `path` for reading. The handle is closed when it goes away.

### function `Create`

```wolang
offer fun Create(path: text) -> File uses io, unsafe
```

Opens `path` for writing, making it or emptying it first.

### function `ReadAll`

```wolang
offer fun ReadAll(path: text, limit: int) -> []u8 uses io, unsafe
```

Reads the whole of `path` into fresh storage.

The caller owns what comes back and gives it to `Free` -- which is
what a `[]u8` is: a view of bytes that are somewhere, and here the
somewhere is an allocation.

### function `WriteAll`

```wolang
offer fun WriteAll(path: text, bytes: []u8) -> bool uses io, unsafe
```

Writes `bytes` to `path`, replacing what was there.

### enum `Kind`

```wolang
offer enum Kind
```

What a path is.

#### method `Missing`

```wolang
Missing
```

#### method `File`

```wolang
File
```

#### method `Directory`

```wolang
Directory
```

#### method `Other`

```wolang
Other
```

### function `KindOf`

```wolang
offer fun KindOf(path: text) -> Kind uses io, unsafe
```

Whether `path` is a file, a directory, something else, or nothing.

### function `Exists`

```wolang
offer fun Exists(path: text) -> bool uses io, unsafe
```

Whether there is anything at `path`.

### function `IsFile`

```wolang
offer fun IsFile(path: text) -> bool uses io, unsafe
```

### function `IsDirectory`

```wolang
offer fun IsDirectory(path: text) -> bool uses io, unsafe
```

### function `SizeOf`

```wolang
offer fun SizeOf(path: text) -> int uses io, unsafe
```

How many bytes the file holds, or -1.

### function `ChangedAt`

```wolang
offer fun ChangedAt(path: text) -> s64 uses io, unsafe
```

When it last changed, as seconds since the epoch, or -1.

### function `Remove`

```wolang
offer fun Remove(path: text) -> bool uses io, unsafe
```

Takes `path` away: a file, or a directory with nothing in it. True
when it is gone afterwards.

### function `Rename`

```wolang
offer fun Rename(from: text, to: text) -> bool uses io, unsafe
```

Moves `from` to `to`, which is also how a file is replaced atomically:
write beside it, then rename over it.

### function `MakeDirectory`

```wolang
offer fun MakeDirectory(path: text, parents: bool) -> bool uses io, unsafe
```

Makes a directory. With `parents`, every one above it too.

### function `List`

```wolang
offer fun List(path: text) -> text uses io, unsafe
```

What is in `path`, as names separated by line breaks.

Names rather than paths, and one string rather than a list: the
caller splits it with `std.text.Pieces`, which is what it would do
with a list anyway -- and this way the whole listing is one
allocation and one call.

### function `Current`

```wolang
offer fun Current() -> text uses io, unsafe
```

The directory the program is running in.

### function `SetCurrent`

```wolang
offer fun SetCurrent(path: text) -> bool uses io, unsafe
```

Moves the program to `path`. False when it is not a directory.

### function `Home`

```wolang
offer fun Home() -> text uses io, unsafe
```

The person's home directory, or "".

### function `TempDirectory`

```wolang
offer fun TempDirectory() -> text uses io, unsafe
```

Where temporary things belong on this machine.

### function `TempName`

```wolang
offer fun TempName(prefix: text) -> text uses io, unsafe
```

A path in the temporary directory that nothing else has.

The name, not the file: a caller that wants it to exist writes to
it, and one that wants a directory calls `MakeDirectory`.

### function `Absolute`

```wolang
offer fun Absolute(path: text) -> text uses io, unsafe
```

`path` with `.` and `..` worked out, whether or not it is there yet.

### function `Copy`

```wolang
offer fun Copy(from: text, to: text, over: bool) -> bool uses io, unsafe
```

Copies a file. `over` says whether an existing one may be replaced.

### function `RemoveTree`

```wolang
offer fun RemoveTree(path: text) -> bool uses io, unsafe
```

Takes a directory and everything in it away.

A separate verb from `Remove`, which takes away one file or one
empty directory: deleting a tree is something a program should have
to say it means.

### function `Walk`

```wolang
offer fun Walk(path: text) -> text uses io, unsafe
```

Every path under `path`, one per line, as `List` answers for one
directory.

### function `Append`

```wolang
offer fun Append(path: text, value: text) -> bool uses io, unsafe
```

Adds to the end of a file, making it when it is not there.

### function `ReadText`

```wolang
offer fun ReadText(path: text, limit: int) -> text uses io, unsafe
```

The whole of a text file.

### function `WriteText`

```wolang
offer fun WriteText(path: text, value: text) -> bool uses io, unsafe
```

Writes text to a file, replacing what was there.

## std.hash

### function `OfBytes`

```wolang
offer fun OfBytes(bytes: []u8) -> u64
```

The hash of `bytes`.

### function `OfText`

```wolang
offer fun OfText(value: text) -> u64 uses unsafe
```

The hash of a string's bytes.

### function `OfInt`

```wolang
offer fun OfInt(value: int) -> u64
```

The hash of a whole number.

Mixed rather than taken as it is: a map that takes the low bits of a
key as its slot puts every multiple of its table size in one place,
and sequential identifiers are the common case.

### function `Combine`

```wolang
offer fun Combine(left: u64, right: u64) -> u64
```

Two hashes joined, for a key made of several parts.

### function `Slot`

```wolang
offer fun Slot(hash: u64, slots: int) -> int
```

The hash as a map wants it: a number from zero to `slots`.

### function `TextHash`

```wolang
offer fun TextHash(value: text) -> int uses unsafe
```

What `std.map` wants for a text key: a hash as an `int`.

### function `TextEquals`

```wolang
offer fun TextEquals(left: text, right: text) -> bool uses unsafe
```

What `std.map` wants for a text key: equality.

## std.heap

### class `Heap`

```wolang
offer class Heap<T>
```

#### field `items`

```wolang
var items: []mut T
```

#### field `count`

```wolang
var count: int = 0
```

#### field `before`

```wolang
var before: fun(T, T) -> bool
```

Whether the first argument comes out before the second.

#### method `deinit`

```wolang
deinit
```

#### method `Len`

```wolang
fun Len() -> int
```

#### method `IsEmpty`

```wolang
fun IsEmpty() -> bool
```

#### method `Peek`

```wolang
fun Peek() -> T
```

What comes out next, without taking it.

#### method `Push`

```wolang
fun Push(value: T) uses unsafe
```

Adds `value`, then moves it up until the order holds.

#### method `Pop`

```wolang
fun Pop() -> T
```

Takes the next one out. The caller checks `Len` first, for the same
reason a deque's `Pop` does.

#### method `Clear`

```wolang
fun Clear()
```

#### method `Reserve`

```wolang
fun Reserve(wanted: int) uses unsafe
```

### function `HeapNew`

```wolang
offer fun HeapNew<T>(before: keep fun(T, T) -> bool) -> Heap<T> uses unsafe
```

## std.ini

### error `Malformed`

```wolang
offer error Malformed
```

What a document that is not one answers with.

### function `Parse`

```wolang
offer fun Parse(input: text) -> Value!Malformed uses unsafe
```

Reads a document. Every value is text.

### function `Write`

```wolang
offer fun Write(value: Value) -> text uses unsafe
```

A value as INI.

Members that are tables become sections and everything else becomes a
pair at the top, which is the only arrangement INI has.

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

### function `Warn`

```wolang
offer fun Warn(message: text) uses io, unsafe
```

Writes `message` to standard error, with a line break.

Its own call rather than a flag on `Print`: a program that mixes the
two wants them to go to different places, and "which stream" is not
something a caller should have to pass every time.

### function `WriteError`

```wolang
offer fun WriteError(message: text) uses io, unsafe
```

Writes `message` to standard error with no line break.

### function `ReadAll`

```wolang
offer fun ReadAll(limit: int) -> text uses io, unsafe
```

Reads up to `limit` bytes of standard input.

Answers what arrived, which may be less: a pipe hands over what it
has. The bytes are a fresh allocation the caller owns.

### function `ReadLine`

```wolang
offer fun ReadLine() -> text? uses io, unsafe
```

Reads one line of standard input, without the line break.

Answers nothing at the end of input, which is what tells a loop
reading commands to stop -- and what "" cannot say, because an empty
line is a line.

### class `Out`

```wolang
offer class Out
```

Collects output and writes it in blocks.

#### field `held`

```wolang
var held: Builder
```

#### field `fd`

```wolang
var fd: s32 = 1
```

#### field `most`

```wolang
var most: int = 65536
```

How much to collect before writing by itself.

#### method `deinit`

```wolang
deinit
```

#### method `Write`

```wolang
fun Write(message: text) uses io, unsafe
```

Adds `message`, writing if there is enough to be worth it.

#### method `Line`

```wolang
fun Line(message: text) uses io, unsafe
```

Adds `message` and a line break.

#### method `Flush`

```wolang
fun Flush() uses io, unsafe
```

Writes what has been collected, if anything.

### function `OutNew`

```wolang
offer fun OutNew() -> Out uses unsafe
```

A writer for standard output.

### function `ErrNew`

```wolang
offer fun ErrNew() -> Out uses unsafe
```

A writer for standard error.

### function `OutTo`

```wolang
offer fun OutTo(fd: s32) -> Out uses unsafe
```

A writer for an open file descriptor.

## std.iter

### ability `Iterator`

```wolang
offer ability Iterator
```

Something that hands out values one at a time.

#### method `Item`

```wolang
type Item
```

#### method `Next`

```wolang
fun Next() -> bool
```

Moves to the next value. False when there is none.

#### method `Value`

```wolang
fun Value() -> Item
```

The value `Next` moved to.

### ability `Iterable`

```wolang
offer ability Iterable
```

Something that can be walked: it hands out an `Iterator`.

#### method `Walker`

```wolang
type Walker: Iterator
```

#### method `Iterate`

```wolang
fun Iterate() -> Walker
```

### function `Count`

```wolang
offer fun Count<C: Iterable>(items: C) -> int
```

How many values `items` has, by walking them.

### function `Collect`

```wolang
offer fun Collect<C: Iterable>(items: C) -> List<C.Walker.Item> uses unsafe
```

Every value of `items`, in a list.

## std.json

### error `Malformed`

```wolang
offer error Malformed(line: int, column: int)
```

What a document that is not one answers with, and where: the line and
the column, both counted from one (D235).

### function `Parse`

```wolang
offer fun Parse(input: text) -> Value!Malformed uses unsafe
```

Reads one value from `input`.

### function `Write`

```wolang
offer fun Write(value: Value) -> text uses unsafe
```

`value` as JSON text.

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
var items: []mut T
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

#### method `Items`

```wolang
fun Items() -> []mut T uses unsafe
```

The elements, as a slice of exactly what is in the list.

A view, not a copy: writing through it writes the list. What every
function taking `[]T` wants, which is most of them. The language
has no slicing syntax yet, so this is a view built from where the
storage is and how much of it is in use.

The view is valid only until the list next grows: `Push` or `Reserve`
past the capacity moves the storage and leaves the view pointing at
freed memory, and so does the list dying. Take it after the last
mutation and use it before the next (D227).

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

### function `Any`

```wolang
offer fun Any<T>(items: []T, holds: fun(T) -> bool) -> bool
```

Whether anything in `items` satisfies `holds`.

### function `All`

```wolang
offer fun All<T>(items: []T, holds: fun(T) -> bool) -> bool
```

Whether everything in `items` does.

### function `Count`

```wolang
offer fun Count<T>(items: []T, holds: fun(T) -> bool) -> int
```

How many satisfy `holds`.

### function `Find`

```wolang
offer fun Find<T>(items: []T, holds: fun(T) -> bool) -> int
```

Where the first one that satisfies `holds` is, or -1.

The index rather than the value: a caller that wants the value has
the slice, and one that wants to write it back needs the index.

### function `Each`

```wolang
offer fun Each<T>(items: []T, body: fun(T))
```

Runs `body` on each element.

### function `Fold`

```wolang
offer fun Fold<T, A>(items: []T, start: A, join: fun(A, T) -> A) -> A
```

Folds `items` into one value, left to right.

### function `MapInPlace`

```wolang
offer fun MapInPlace<T>(items: []mut T, change: fun(T) -> T)
```

Replaces each element with what `change` makes of it, in place.

In place rather than into a new list: a list that owns its storage is
something to free, and a caller that wants both keeps a copy.

### function `Keep`

```wolang
offer fun Keep<T>(items: []mut T, holds: fun(T) -> bool) -> int
```

Moves everything that satisfies `holds` to the front, and answers how
many there are.

What filtering is without allocating: the caller takes the first `n`
elements, and what was dropped is past them.

## std.log

### enum `Level`

```wolang
offer enum Level
```

How much to say.

#### method `Trace`

```wolang
Trace
```

#### method `Debug`

```wolang
Debug
```

#### method `Info`

```wolang
Info
```

#### method `Warning`

```wolang
Warning
```

#### method `Error`

```wolang
Error
```

#### method `Silent`

```wolang
Silent
```

### function `SetLevel`

```wolang
offer fun SetLevel(level: Level)
```

Says nothing below `level` from here on.

### function `Enabled`

```wolang
offer fun Enabled(level: Level) -> bool
```

Whether `level` would be written. For a caller whose message costs
something to build: the check is cheaper than the string.

### function `SetTimestamps`

```wolang
offer fun SetTimestamps(on: bool)
```

Turns the timestamp off, for a program whose output is compared.

### function `At`

```wolang
offer fun At(level: Level, message: text) uses io, time, unsafe
```

Writes one line at `level`.

### function `Trace`

```wolang
offer fun Trace(message: text) uses io, time, unsafe
```

### function `Debug`

```wolang
offer fun Debug(message: text) uses io, time, unsafe
```

### function `Info`

```wolang
offer fun Info(message: text) uses io, time, unsafe
```

### function `Warning`

```wolang
offer fun Warning(message: text) uses io, time, unsafe
```

### function `Error`

```wolang
offer fun Error(message: text) uses io, time, unsafe
```

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
var keys: []mut K
```

#### field `values`

```wolang
var values: []mut V
```

#### field `filled`

```wolang
var filled: []mut bool
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
offer fun MapNew<K, V>(hash: keep fun(K) -> int, equals: keep fun(K, K) -> bool) -> Map<K, V> uses unsafe
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

### function `IntMapNew`

```wolang
offer fun IntMapNew<V>() -> Map<int, V> uses unsafe
```

A map keyed by `int`, with the hashing already chosen.

### function `TextMapNew`

```wolang
offer fun TextMapNew<V>() -> Map<text, V> uses unsafe
```

A map keyed by `text`: what a symbol table is (D250).

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
offer unsafe fun BytesFromRaw(data: *u8, length: int) -> []mut u8
```

A view of `length` bytes at `data`.

`unsafe` because nothing here can check that there are that many, or
that they stay there. The caller promises both.

### function `SoaNew`

```wolang
offer unsafe fun SoaNew<T>(count: int) -> soa []mut T
```

Storage for `count` records, laid out as one array per field.

`soa []T` is the same record with a different layout: a walk over one
field touches every value in a cache line instead of every seventh.
Which is the whole point, and why this is a layout rather than a type.

Like `BytesFromRaw`, it has no body: how big the storage is, is a
question only a tier can answer, so the compiler answers it at the call.

### function `ArrayNew`

```wolang
offer unsafe fun ArrayNew<T>(count: int) -> []mut T
```

Storage for `count` records, one after another.

The same records as `SoaNew` and a different layout: this is the one
that reads well when a pass touches most of a record, and the other is
the one that reads well when it touches one field of it.

### function `Free`

```wolang
offer unsafe fun Free<T>(items: []T)
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
offer unsafe fun ViewFromRaw<T>(data: *u8, count: int) -> []mut T
```

A view of `count` values of `T` at `data`.

`BytesFromRaw` with an element type: the same pair, and the same
reason for having no body. `unsafe` because nothing here can check
that there are that many values there, or that they stay.

### function `Prefix`

```wolang
offer unsafe fun Prefix<T>(items: []T, count: int) -> []T
```

The first `count` elements of `items`, as a view.

A view and not a copy: writing through it writes the storage. What a
container with room to spare hands out -- `List.Items()` is this with
the count filled in -- and the one place that knows a slice is a
pointer and a length (D138).

### function `PrefixMut`

```wolang
offer unsafe fun PrefixMut<T>(items: []mut T, count: int) -> []mut T
```

`Prefix` of a view that may be written, as a view that may be written
(D251). The same instruction: only what the caller is promised differs.

### function `SizeOf`

```wolang
offer fun SizeOf<T>(value: T) -> int
```

How many bytes a value of `value`'s type takes.

The value is not read -- what is wanted is its type, which is why this
is spelled with an argument. How big a value is depends on how the
tier laid it out, so the compiler answers at the call (D100).

### function `Suffix`

```wolang
offer unsafe fun Suffix<T>(items: []T, at: int) -> []T
```

The elements of `items` from `at` onwards, as a view.

The other half of `Prefix`, and an intrinsic for the same reason: a
view is a pointer and a length, and only the tiers know how far
along an element is. Between them a caller can name any run inside a
view without copying it (D177).

## std.net

### class `Socket`

```wolang
offer class Socket
```

One connection, open until it is closed.

#### field `fd`

```wolang
var fd: s32 = -1
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Send`

```wolang
fun Send(message: text) -> bool uses unsafe
```

Sends everything in `message`. False when the other end went away.

#### method `Receive`

```wolang
fun Receive(limit: int) -> text uses unsafe
```

Reads up to `limit` bytes. "" when the other end closed.

#### method `ReceiveAll`

```wolang
fun ReceiveAll(limit: int) -> text uses unsafe
```

Reads until the other end closes or `limit` bytes have arrived.

#### method `Close`

```wolang
fun Close() uses unsafe
```

#### method `SetNoDelay`

```wolang
fun SetNoDelay(on: bool) uses unsafe
```

Whether small writes go out at once rather than waiting for more.
What a server turns off, because a reply held back is latency
nobody asked for.

#### method `SetBlocking`

```wolang
fun SetBlocking(on: bool) uses unsafe
```

Whether a call that would wait answers instead.

### function `ParseAddress`

```wolang
offer fun ParseAddress(host: text) -> s64? uses unsafe
```

The number a dotted address spells, or nothing.

No name lookup: a resolver is a protocol of its own, and a program
that needs one should say so rather than have it happen inside a
connect.

### function `Resolve`

```wolang
offer fun Resolve(host: text) -> s64? uses net, unsafe
```

The address `host` names, whether it is written as numbers or as a
name.

A dotted address is read here; a name goes to the system's resolver,
because that is what every other program on the machine uses (D148).

### function `Connect`

```wolang
offer fun Connect(host: text, port: int) -> Socket uses net, unsafe
```

Connects to `host` on `port`. A closed socket when it fails.

### function `Listen`

```wolang
offer fun Listen(port: int, backlog: int) -> Socket uses net, unsafe
```

A socket listening on `port`, or a closed one.

### function `Accept`

```wolang
offer fun Accept(listener: Socket) -> Socket uses net, unsafe
```

Waits for a connection on a listening socket.

### struct `Datagram`

```wolang
offer struct Datagram
```

One datagram received: how many bytes landed in the caller's buffer,
who sent it, and whether it was longer than the buffer (then the rest
is gone, as UDP has no way to ask for it again).

#### field `count`

```wolang
var count: int
```

#### field `address`

```wolang
var address: s64
```

#### field `port`

```wolang
var port: int
```

#### field `truncated`

```wolang
var truncated: bool
```

### class `UdpSocket`

```wolang
offer class UdpSocket
```

An unconnected datagram socket (D244). Its `fd` can be given to a
poller like any other descriptor.

#### field `fd`

```wolang
var fd: s32 = -1
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `LocalPort`

```wolang
fun LocalPort() -> int uses net, unsafe
```

The port the socket is bound to; how port 0 says which was picked.
-1 when closed.

#### method `SendTo`

```wolang
fun SendTo(address: s64, port: int, bytes: []u8) -> int uses net, unsafe
```

Sends `bytes` as one datagram to `address` (see `Resolve`) and
`port`. The bytes sent, or -1: closed socket, no bytes, or more than
65507 (the most a datagram carries).

#### method `SendTextTo`

```wolang
fun SendTextTo(address: s64, port: int, message: text) -> int uses net, unsafe
```

Sends the bytes of `message`; see `SendTo`.

#### method `ReceiveFrom`

```wolang
fun ReceiveFrom(buffer: []mut u8) -> Datagram? uses net, unsafe
```

Takes one datagram into `buffer`, never writing past its length. Null
when a non-blocking socket has nothing yet, or on an error; a
datagram longer than the buffer comes with `truncated` set.

#### method `SetNonBlocking`

```wolang
fun SetNonBlocking(on: bool) uses net, unsafe
```

Whether `ReceiveFrom` answers null at once (true) or waits (the
default).

#### method `SetBroadcast`

```wolang
fun SetBroadcast(on: bool) -> bool uses net, unsafe
```

Whether datagrams may be sent to the broadcast address.

#### method `Close`

```wolang
fun Close() uses net, unsafe
```

### function `BindUdp`

```wolang
offer fun BindUdp(port: int) -> UdpSocket uses net, unsafe
```

A UDP socket bound to `port` on every interface (0 picks a free one;
ask `LocalPort`), or a closed one.

### function `BindUdpOn`

```wolang
offer fun BindUdpOn(address: s64, port: int) -> UdpSocket uses net, unsafe
```

Like `BindUdp` but on one local `address` (e.g. 127.0.0.1 from
`ParseAddress`), so a test or a LAN-only game stays off other
interfaces.

## std.ops

### ability `Add`

```wolang
@Operator
offer ability Add
```

`a + b` and `a += b`.

#### method `Plus`

```wolang
fun Plus(other: Self) -> Self
```

### ability `Sub`

```wolang
@Operator
offer ability Sub
```

`a - b` and `a -= b`.

#### method `Minus`

```wolang
fun Minus(other: Self) -> Self
```

### ability `Mul`

```wolang
@Operator
offer ability Mul
```

`a * b` and `a *= b`.

#### method `Times`

```wolang
fun Times(other: Self) -> Self
```

### ability `Div`

```wolang
@Operator
offer ability Div
```

`a / b` and `a /= b`.

#### method `Divide`

```wolang
fun Divide(other: Self) -> Self
```

### ability `Neg`

```wolang
@Operator
offer ability Neg
```

`-a`.

#### method `Negate`

```wolang
fun Negate() -> Self
```

### ability `Eq`

```wolang
@Operator
offer ability Eq
```

`a == b` and `a != b`. The other side is borrowed, so comparing two
counted values neither copies nor frees either.

#### method `Equals`

```wolang
fun Equals(other: &Self) -> bool
```

### ability `Ord`

```wolang
@Operator
offer ability Ord
```

`a < b`, `a <= b`, `a > b` and `a >= b`: `Compare` answers a negative
number, zero or a positive one for less, equal and greater.

#### method `Compare`

```wolang
fun Compare(other: &Self) -> int
```

### ability `Hash`

```wolang
offer ability Hash
```

A number a table can file the value under. Not an operator: it is what
`std.map` and `std.set` ask of a key that is not a number or text.

#### method `Hash`

```wolang
fun Hash() -> int
```

### function `Max`

```wolang
offer fun Max<T: Ord>(a: T, b: T) -> T
```

The greater of two, by `Ord`.

### function `Min`

```wolang
offer fun Min<T: Ord>(a: T, b: T) -> T
```

The lesser of two, by `Ord`.

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

## std.path

### function `IsAbsolute`

```wolang
offer fun IsAbsolute(path: text) -> bool uses unsafe
```

Whether `path` starts from the root.

### function `Name`

```wolang
offer fun Name(path: text) -> text uses unsafe
```

The last part of `path`: `a/b/c.wo` is `c.wo`.

### function `Parent`

```wolang
offer fun Parent(path: text) -> text uses unsafe
```

Everything before the last separator: `a/b/c.wo` is `a/b`.

"" for a name with no separator in it, and "/" for a path directly
under the root -- both of which a caller joining onto the answer
wants.

### function `Extension`

```wolang
offer fun Extension(path: text) -> text uses unsafe
```

The extension of the last part, without the dot: `c.wo` is `wo`.

"" when there is none, and "" for a name that is only a dot -- a
leading dot is a hidden file rather than an extension.

### function `Stem`

```wolang
offer fun Stem(path: text) -> text uses unsafe
```

The last part without its extension: `a/b/c.wo` is `c`.

### function `Join`

```wolang
offer fun Join(left: text, right: text) -> text uses unsafe
```

`left` and `right` with one separator between them.

An absolute `right` wins, which is what every path library does and
what a caller joining a configured directory with a configured file
depends on.

### function `Normalize`

```wolang
offer fun Normalize(path: text) -> text uses unsafe
```

`path` with `.` and `..` worked out, and double separators removed.

Textual: it never looks at the filesystem, so a `..` after a symbolic
link means what the text says rather than what the disk says.

A path with more than 64 parts left after the dots are worked out
answers "" -- the one thing Normalize otherwise never returns -- rather
than dropping the rest, which would turn a deep path into a different,
shallower one (D223).

### function `IsInside`

```wolang
offer fun IsInside(path: text, directory: text) -> bool uses unsafe
```

Whether `path` is inside `directory`, textually.

### function `WithExtension`

```wolang
offer fun WithExtension(path: text, extension: text) -> text uses unsafe
```

`path` with a different extension. An empty `extension` removes it.

## std.process

### function `Exit`

```wolang
offer fun Exit(code: int) uses io, unsafe
```

Stops the program with `code`. Nothing after this runs.

Destructors do not run: a program that has to tidy up should return
from `main` instead, and this is for the case where there is nothing
left to tidy.

### function `Id`

```wolang
offer fun Id() -> int uses io, unsafe
```

This program's identifier.

### function `Run`

```wolang
offer fun Run(command: text) -> int uses io, unsafe
```

Runs `command` through the system shell and answers its exit code.

`unsafe` because the command is a string: what is in it is the
caller's responsibility, and a command built from input a program did
not choose is the oldest hole there is.

### function `Sleep`

```wolang
offer fun Sleep(millis: int) uses time, unsafe
```

Sleeps for `millis` milliseconds.

### struct `Said`

```wolang
offer struct Said
```

What another program did.

#### field `code`

```wolang
var code: int = -1
```

Its exit code, or a negative number when the system stopped it or
it could not be started at all.

#### field `out`

```wolang
var out: text = ""
```

#### field `errors`

```wolang
var errors: text = ""
```

### constant `kMostOutput`

```wolang
offer const kMostOutput: int = 1048576
```

How much of each stream to keep. A program that says more than this
has the rest dropped, which beats a tool that grows without bound.

### function `Start`

```wolang
offer fun Start(command: text) -> Said uses io, unsafe
```

Runs `command` and collects what it said.

No shell: the command is split on blanks with quoted runs kept
whole, and the program is started directly. Which is the difference
between running `git` and running whatever a `;` in an argument says
(D153).

### function `StartWith`

```wolang
offer fun StartWith(command: text, input: text) -> Said uses io, unsafe
```

The same, with `input` written to the program's standard input.

### function `Works`

```wolang
offer fun Works(command: text) -> bool uses io, unsafe
```

Whether `command` exists and ran without complaint.

### struct `Command`

```wolang
offer struct Command
```

Everything a caller may say about how a program runs.

Built up and handed to `StartCommand`, the way an HTTP call is
built up and handed to `Perform` (D151):
the command, what to write to it, where to run it, and what to add
to its environment.

#### field `command`

```wolang
var command: text = ""
```

#### field `input`

```wolang
var input: text = ""
```

#### field `directory`

```wolang
var directory: text = ""
```

#### field `environment`

```wolang
var environment: text = ""
```

`NAME=value` lines, added to what this process has rather than
replacing it -- a child with no `PATH` cannot find anything.

### function `CommandNew`

```wolang
offer fun CommandNew(command: text) -> Command
```

### function `WithInput`

```wolang
offer fun WithInput(run: Command, input: text) -> Command
```

### function `WithDirectory`

```wolang
offer fun WithDirectory(run: Command, directory: text) -> Command
```

### function `WithVariable`

```wolang
offer fun WithVariable(run: Command, name: text, value: text) -> Command uses unsafe
```

One more `NAME=value` for the child.

### function `StartCommand`

```wolang
offer fun StartCommand(run: Command) -> Said uses io, unsafe
```

Runs what the `Command` describes.

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
offer fun RandomFromClock() -> Random uses time, unsafe
```

A generator seeded from the clock.

## std.regex

### error `BadPattern`

```wolang
offer error BadPattern
```

What a pattern that is not one answers with.

### class `Regex`

```wolang
offer class Regex
```

A compiled pattern.

#### field `steps`

```wolang
var steps: List<Step>
```

#### field `groups`

```wolang
var groups: int = 0
```

#### field `fold`

```wolang
var fold: bool = false
```

#### field `dot_all`

```wolang
var dot_all: bool = false
```

#### method `Groups`

```wolang
fun Groups() -> int
```

### struct `Match`

```wolang
offer struct Match
```

Where a match is: the whole of it, and each group.

`start` and `end` are byte offsets; a group that did not take part
has -1 for both.

#### field `start`

```wolang
var start: int = -1
```

#### field `end`

```wolang
var end: int = -1
```

#### field `starts`

```wolang
var starts: [20]int
```

#### field `ends`

```wolang
var ends: [20]int
```

#### field `ok`

```wolang
var ok: bool = false
```

### function `MatchNone`

```wolang
offer fun MatchNone() -> Match
```

### function `Whole`

```wolang
offer fun Whole(input: text, found: &Match) -> text uses unsafe
```

The text a match covers.

### function `Group`

```wolang
offer fun Group(input: text, found: &Match, index: int) -> text uses unsafe
```

The text group `index` covers, or "" when it took no part. Group 1 is
the first `(`.

### function `Compile`

```wolang
offer fun Compile(pattern: text, flags: text) -> Regex!BadPattern uses unsafe
```

Compiles `pattern`. `flags` holds `i` and `s`.

### function `Find`

```wolang
offer fun Find(pattern: &Regex, input: text) -> Match uses unsafe
```

Where `pattern` first matches in `input`, or a match that is not one.

### function `FindFrom`

```wolang
offer fun FindFrom(pattern: &Regex, input: text, from: int) -> Match uses unsafe
```

The same, starting at `from`.

### function `Matches`

```wolang
offer fun Matches(pattern: &Regex, input: text) -> bool uses unsafe
```

Whether `pattern` matches anywhere in `input`.

### function `FindAll`

```wolang
offer fun FindAll(pattern: &Regex, input: text) -> List<Match> uses unsafe
```

Every match, left to right, without overlapping.

### function `ReplaceAll`

```wolang
offer fun ReplaceAll(pattern: &Regex, input: text, with: text) -> text uses unsafe
```

`input` with every match replaced by `with`.

`$1` in `with` is the first group, `$0` the whole match, and `$$` a
dollar sign.

### function `Split`

```wolang
offer fun Split(pattern: &Regex, input: text) -> List<text> uses unsafe
```

`input` split on every match.

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
offer fun SetNew<T>(hash: keep fun(T) -> int, equals: keep fun(T, T) -> bool) -> Set<T> uses unsafe
```

## std.sort

### function `Sort`

```wolang
offer fun Sort<T>(items: []mut T, before: fun(T, T) -> bool)
```

Sorts `items` in place, with `before(a, b)` saying whether `a` comes
first.

The comparison has to be a strict order: `before(a, a)` false, and
`before(a, b)` and `before(b, a)` never both true. A comparison that
is not one leaves the elements in some order rather than looping.

### function `SortBy`

```wolang
offer fun SortBy<T: Ord>(items: []mut T)
```

Sorts `items` in place by their own order, the one `<` gives them
(D239): `Sort` with the comparison already written.

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

The bytes of `value`, as a view. Nothing is copied. The view is
read-only: it is of text that may be a literal, and `[]u8` cannot be
written through (D251).

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
var bytes: []mut u8
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

The view is valid only until the next `Append`, `Reserve` or `Clear`
(they may move or rewrite the bytes) and until the builder dies (its
destructor frees them). Returning it from the function that owns the
builder, or keeping it after, dangles. A string that has to outlive
the builder is `Release`, the owning one (D227).

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

#### method `AppendLine`

```wolang
fun AppendLine(value: text) uses unsafe
```

`value` and a newline: what a printer that builds a dump line by line writes (D250).

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
offer unsafe fun TextFromRaw(data: *u8, length: int) -> text
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
offer fun PieceAt(value: text, separator: text, which: int) -> text uses unsafe
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
offer fun Replace(value: text, needle: text, with: text) -> text uses unsafe
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
@Thread
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
offer unsafe fun AtomicAdd(counter: *mut s64, delta: s64) -> s64
```

Adds `delta` to the number at `counter` and gives back what was there
before, with no other thread able to see a half-done sum.

### function `ParallelFor`

```wolang
@Thread(captures)
offer unsafe fun ParallelFor(count: int, threads: int, body: fun(int, int))
```

Runs `body` over `[0, count)` on `threads` threads, in chunks.

Each call gets a range to itself -- `body(begin, end)` -- so the body
is an ordinary loop and the work is split without any locking. Zero
threads means one per core, and the calling thread takes the last
chunk, so a one-thread loop spawns nothing (D147).

The body may be a closure, which is the point: `ParallelFor(n, 0,
|from, to| ...)` takes what it needs with it. What it may not do is
write to what it captured -- a capture is a copy, and here there is
one copy per thread -- so a result comes back through storage the
body was *given*, by index.

Declared with no body: what the runtime needs is the two halves of
the body's function value, and lowering is the only place that can
take a function value apart (D147).

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
offer fun Now() -> Instant uses time, unsafe
```

How long the machine has been up, as seconds and nanoseconds.

Monotonic: the difference between two of these is a duration, and it
is never negative no matter what happens to the system clock.

### function `WallClock`

```wolang
offer fun WallClock() -> Instant uses time, unsafe
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

### struct `DateTime`

```wolang
offer struct DateTime
```

A moment taken apart, in UTC.

No time zones: they are a database that changes four times a year, and
a program that needs one should read that database rather than have
this guess. UTC is what a log line and a wire format want anyway.

#### field `year`

```wolang
var year: int = 1970
```

#### field `month`

```wolang
var month: int = 1
```

#### field `day`

```wolang
var day: int = 1
```

#### field `hour`

```wolang
var hour: int = 0
```

#### field `minute`

```wolang
var minute: int = 0
```

#### field `second`

```wolang
var second: int = 0
```

#### field `weekday`

```wolang
var weekday: int = 4
```

#### field `yearday`

```wolang
var yearday: int = 1
```

### function `Civil`

```wolang
offer fun Civil(seconds: s64) -> DateTime
```

`seconds` since the epoch, taken apart.

The civil-from-days algorithm: it shifts the year to start in March so
that the leap day lands at the end, which is what removes every
special case from the month arithmetic.

### function `YearDay`

```wolang
offer fun YearDay(year: int, month: int, day: int) -> int
```

Which day of the year a date is, from one.

### function `IsLeapYear`

```wolang
offer fun IsLeapYear(year: int) -> bool
```

Whether `year` has a 29th of February.

### function `Epoch`

```wolang
offer fun Epoch(moment: DateTime) -> s64
```

Seconds since the epoch for a date in UTC.

### function `Iso`

```wolang
offer fun Iso(moment: DateTime) -> text uses unsafe
```

`2026-10-15T14:03:07Z`, which sorts as text and parses everywhere.

### function `Clock`

```wolang
offer fun Clock(moment: DateTime, millis: int) -> text uses unsafe
```

`14:03:07.123`, which is what a log line wants.

### function `WeekdayName`

```wolang
offer fun WeekdayName(weekday: int) -> text
```

The three-letter name of a weekday, from Sunday.

### function `MonthName`

```wolang
offer fun MonthName(month: int) -> text
```

The three-letter name of a month, from one.

### function `HttpDate`

```wolang
offer fun HttpDate(seconds: s64) -> text uses unsafe
```

`Wed, 15 Oct 2026 14:03:07 GMT`: the date format HTTP uses.

### function `ZoneOffset`

```wolang
offer fun ZoneOffset(seconds: s64, zone: text) -> int uses time, unsafe
```

How many seconds `zone` is ahead of UTC at that moment.

The moment matters: half the world moves its clocks twice a year.
An empty zone is the machine's own. `"Europe/Kyiv"`, `"UTC"`,
`"America/New_York"` -- whatever the system's database holds.

### function `ZoneName`

```wolang
offer fun ZoneName(seconds: s64, zone: text) -> text uses time, unsafe
```

What the zone is called at that moment: `CET`, `PDT`, `EEST`.

### function `CivilIn`

```wolang
offer fun CivilIn(seconds: s64, zone: text) -> DateTime uses time, unsafe
```

The same moment, as the wall clock reads in `zone`.

What a program shows a person: the moment is the same, the numbers
are theirs.

### function `EpochIn`

```wolang
offer fun EpochIn(moment: DateTime, zone: text) -> s64 uses time, unsafe
```

The moment a wall clock in `zone` was showing.

Two passes: the offset depends on the moment, and the moment is what
is being worked out. The second pass is what gets the hour after a
clock change right.

### function `ParseIso`

```wolang
offer fun ParseIso(value: text) -> s64? uses unsafe
```

Reads `2026-10-15T18:09:23Z` and the shapes around it.

What it takes: a date, optionally a time after `T` or a blank,
optionally a fraction, optionally `Z` or `+02:00`. What it answers:
seconds since the epoch, or nothing when the text is not a moment.
The one format worth having built in, because it is the one every
machine writes (D176).

### function `IsoInstant`

```wolang
offer fun IsoInstant(seconds: s64) -> text uses unsafe
```

`2026-10-15T18:09:23Z` from a moment, which is what `ParseIso` reads
back. `Iso` is the same thing from a `DateTime`.

### function `Ago`

```wolang
offer fun Ago(seconds: s64, now: s64) -> text uses unsafe
```

How long ago, in words a person reads: `just now`, `3 minutes ago`,
`2 days ago`. What a listing shows instead of a timestamp.

## std.toml

### error `Malformed`

```wolang
offer error Malformed(line: int, column: int)
```

What a document that is not one answers with, and where: the line and
the column, both counted from one (D235).

### function `Parse`

```wolang
offer fun Parse(input: text) -> Value!Malformed uses unsafe
```

Reads a document.

### function `NumberOf`

```wolang
offer fun NumberOf(spelled: text) -> float? uses unsafe
```

The number `spelled` is, in any of TOML's four bases, or nothing.

### function `Write`

```wolang
offer fun Write(value: Value) -> text uses unsafe
```

A value as TOML.

Tables are written as headers and arrays of tables as double headers,
which is what a person writing a manifest by hand would have written.

## std.utf8

### function `Width`

```wolang
offer fun Width(first: u8) -> int
```

How many bytes the code point starting at `first` takes.

Zero for a byte that cannot start one, which is what tells a walk to
stop rather than to loop.

### function `Count`

```wolang
offer fun Count(value: text) -> int uses unsafe
```

How many code points `value` holds.

### function `IsValid`

```wolang
offer fun IsValid(value: text) -> bool uses unsafe
```

Whether `value` is valid UTF-8 throughout.

### struct `Char`

```wolang
offer struct Char
```

One code point and where the next one starts.

#### field `code`

```wolang
var code: int
```

#### field `at`

```wolang
var at: int
```

#### field `width`

```wolang
var width: int
```

### function `CharAt`

```wolang
offer fun CharAt(value: text, at: int) -> Char uses unsafe
```

The code point starting at byte `at`.

### class `Chars`

```wolang
offer class Chars
```

A walk over the code points of a string.

#### field `value`

```wolang
var value: text
```

#### field `at`

```wolang
var at: int = 0
```

#### field `current`

```wolang
var current: Char = Char(0, 0, 0)
```

#### method `Iterate`

```wolang
fun Iterate() -> Chars
```

#### method `Next`

```wolang
fun Next() -> bool uses unsafe
```

#### method `Value`

```wolang
fun Value() -> int
```

### function `Walk`

```wolang
offer fun Walk(value: text) -> Chars
```

The code points of `value`, for `for code in Walk(value)`.

### function `Encode`

```wolang
offer fun Encode(code: int) -> text uses unsafe
```

One code point as text. Allocates.

### function `AppendTo`

```wolang
offer fun AppendTo(builder: &mut Builder, code: int) uses unsafe
```

Writes one code point into a builder.

## std.yaml

### error `Malformed`

```wolang
offer error Malformed(line: int, column: int)
```

What a document that is not one answers with, and the line it went wrong
on and a column in it, both counted from one (D235).

### function `Parse`

```wolang
offer fun Parse(input: text) -> Value!Malformed uses unsafe
```

Reads the first document of `input`.

### function `ParseAll`

```wolang
offer fun ParseAll(input: text) -> List<Value>!Malformed uses unsafe
```

Every document in a stream, which is what `---` separates.

### function `ReadScalar`

```wolang
offer fun ReadScalar(value: text) -> Step uses unsafe
```

What a scalar on one line is, with the core schema applied.

### function `Resolve`

```wolang
offer fun Resolve(held: text) -> Value uses unsafe
```

A plain scalar, as the core schema reads it.

### function `NumberOf`

```wolang
offer fun NumberOf(spelled: text) -> float? uses unsafe
```

The number `spelled` is, in any base YAML allows, or nothing.

### function `Write`

```wolang
offer fun Write(value: Value) -> text uses unsafe
```

A value as YAML.

Block style, two spaces an indent, which is what a person writing it
by hand would have written.
