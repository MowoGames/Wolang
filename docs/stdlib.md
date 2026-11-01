# The standard library

Thirty-five modules, all written in Wolang over a floor of about sixty C
calls. This is what each one is for, what it gives you, and what it does
not. Every example here compiles.

A length, an index and a hash are `int`, which is `s64` (D238): `.len` on a
text, slice or list is an `int`, and a collection is not capped at 2^31.

How to read a signature: `uses unsafe` on a standard library function
means *it* takes the responsibility, not you — `std.math.Sqrt`-style
wrappers over a C call. The one exception is `std.text.Bytes`: it hands
out a writable view of text that may be a literal, so a caller declares
`uses unsafe` and answers for never writing through it (D233).

| Module | What it is for |
|---|---|
| [`std.text`](#stdtext) | Strings: searching, slicing, splitting, building |
| [`std.fmt`](#stdfmt) | Numbers as text, in bases, columns and units |
| [`std.utf8`](#stdutf8) | Characters rather than bytes |
| [`std.regex`](#stdregex) | Patterns, with groups and replacement |
| [`std.math`](#stdmath) | The usual functions, and the wrapping ones |
| [`std.list`](#stdlist) | A growing sequence, and what to do to one |
| [`std.map`](#stdmap), [`std.set`](#stdset) | Hashed lookups |
| [`std.deque`](#stddeque), [`std.heap`](#stdheap) | A queue at both ends, and a priority queue |
| [`std.sort`](#stdsort) | Sorting and binary search |
| [`std.doc`](#stddoc) | What a document is, whatever it was written in |
| [`std.json`](#stdjson), [`std.toml`](#stdtoml), [`std.yaml`](#stdyaml), [`std.ini`](#stdini), [`std.csv`](#stdcsv) | Five formats, one tree |
| [`std.io`](#stdio) | Standard input, output and error |
| [`std.fs`](#stdfs) | Files, directories and trees |
| [`std.path`](#stdpath) | Joining and taking paths apart |
| [`std.net`](#stdnet) | Sockets, and names |
| [`std.process`](#stdprocess) | Running another program |
| [`std.env`](#stdenv) | The environment and the command line |
| [`std.time`](#stdtime) | Moments, calendars, zones and durations |
| [`std.crypto`](#stdcrypto) | Hashes, signatures, randomness, identifiers |
| [`std.compress`](#stdcompress) | Deflate, gzip and CRC-32 |
| [`std.encoding`](#stdencoding) | Base64, hex and URL escaping |
| [`std.hash`](#stdhash) | Fast hashing for tables |
| [`std.random`](#stdrandom) | A generator you can repeat |
| [`std.log`](#stdlog) | Levelled logging |
| [`std.thread`](#stdthread), [`std.tasks`](#stdtasks) | Threads, locks, and a frame's worth of parallel work |
| [`std.mem`](#stdmem), [`std.own`](#stdown) | Storage, views and ownership |
| [`std.ops`](#stdops) | Operators as abilities: `Add`, `Eq`, `Ord` and the rest |

And three first-party packages beside it: [`http`](#the-http-package),
[`db`](#the-db-package), [`gfx`](#the-gfx-package), plus `tls` and
`game`.

---

## std.text

Searching, slicing, splitting, joining, padding, parsing.

```wolang
need std.io.{Print}
need std.text.{BuilderNew, Contains, Join, PadStart, Pieces, Replace,
               Split, Substring, Trim, Upper}

fun main() uses io, unsafe {
  const line = "  wolang,fast,typed  "
  const held = Trim(line)
  Print(Upper(Substring(held, 0, 6)))        // WOLANG
  Print("{Contains(held, "fast")}")          // true
  Print(Replace(held, ",", " / "))           // wolang / fast / typed
  Print(PadStart("7", 3, "0"))               // 007

  // Splitting walks rather than allocating a list.
  for piece in Pieces(held, ",") {
    Print("piece: {piece}")
  }

  // Building a string is a builder, which grows once rather than
  // copying per `+`.
  var builder = BuilderNew()
  var i = 0
  while i < 3 {
    builder.Append("{i} ")
    i = i + 1
  }
  Print(builder.Release())                   // 0 1 2
}
```

`builder.Text()` is a view of the builder's own bytes, valid only until the
next `Append`, `Reserve` or `Clear` and until the builder dies; returning it
from the function that owns the builder dangles. `Release()` is the owning
form: use it for any string that outlives the builder (D227).

`Length`, `IsEmpty`, `StartsWith`, `EndsWith`, `IndexOf`, `LastIndexOf`,
`After`, `Before`, `TrimStart`, `TrimEnd`, `CountPieces`, `PieceAt`,
`Repeat`, `PadEnd`, `Lower`, `ParseInt`, `ParseFloat`, `Bytes`,
`TextFromRaw`.

A `text` is bytes and a length. There is no copy on a substring and no
terminator; `Bytes` (`uses unsafe`, a view you must not write) hands you the bytes and `TextFromRaw` takes them
back.

## std.fmt

```wolang
need std.fmt.{Binary, Bytes, Duration, Float, Hex, Left, Percent,
              Right}
need std.io.{Print}

fun main() uses io, unsafe {
  Print(Hex(48879))             // beef
  Print(Binary(10))             // 1010
  Print(Float(3.14159, 2))      // 3.14
  Print(Bytes(1536))            // 1.5 KB
  Print(Duration(90000))        // 1m 30s
  Print(Percent(0.42, 1))       // 42.0%
  Print(Left("name", 12) + Right("42", 6))
}
```

## std.utf8

Bytes are not characters. This module is where that distinction lives.

```wolang
need std.io.{Print}
need std.utf8.{CharAt, Count, Encode, IsValid, Walk}

fun main() uses io, unsafe {
  const held = "привет"
  Print("{Count(held)} characters, {held.len} bytes")   // 6, 12
  Print("{IsValid(held)}")
  // `Walk` yields code points; `CharAt` gives the whole thing -- the
  // point, where it starts and how wide it is.
  for code in Walk(held) {
    Print("{code}")
  }
  const first = CharAt(held, 0)
  Print("the first is {first.code}, {first.width} bytes wide")
  Print(Encode(0x1F600))         // 😀
}
```

## std.regex

A backtracking matcher with a budget: the famous exponential patterns
answer "no" rather than never answering. `Compile` answers `BadPattern` for
what would blow up: groups nested more than 32 deep, a repeat count over
1000, or a program over 20000 steps (D227).

```wolang
need std.io.{Print}
need std.regex.{Compile, Find, FindAll, Group, Matches, ReplaceAll,
                Split, Whole}

fun main() uses io, unsafe {
  match Compile("(\\w+)@(\\w+)\\.com", "i") {
    Ok(pattern) => {
      const line = "write to Someone@Example.com today"
      const found = Find(&pattern, line)
      Print(Whole(line, &found))          // Someone@Example.com
      Print(Group(line, &found, 1))       // Someone
      Print(ReplaceAll(&pattern, line, "<$1>"))
      Print("{Matches(&pattern, "nothing here")}")   // false
    },
    BadPattern => { Print("not a pattern") },
  }
}
```

Supported: literals, `.`, classes with ranges and negation, `\d \w \s`
and their capitals, `^ $ \b \B`, groups and non-capturing groups,
alternation, `* + ? {n} {n,} {n,m}` and their reluctant forms, the flags
`i` and `s`. Not supported: look-around and back-references.

## std.math

```wolang
need std.io.{Print}
need std.math.{Clamp, Lerp, Max, Near, Pi, Sqrt, WrappingAdd}

fun main() uses io, unsafe {
  Print("{Sqrt(2.0)}")
  Print("{Clamp(17, 0, 10)}")          // 10
  Print("{Lerp(0.0, 10.0, 0.25)}")     // 2.5
  Print("{Near(0.1 + 0.2, 0.3, 1e-9)}")
  // Every operator traps on overflow; these are how you ask for the
  // other behaviour on purpose.
  Print("{WrappingAdd(18446744073709551615, 1)}")   // 0
}
```

## std.iter

What being walked means, as two abilities: `Iterator` hands out values
(`type Item`, `Next`, `Value`) and `Iterable` hands out an iterator
(`type Walker: Iterator`, `Iterate`). `List`, `Deque`, `Map`, `Split` and
`Chars` implement both, so generic code over `C: Iterable` walks any of
them. `Count(items)` and `Collect(items)` are written that way.

## std.list

```wolang
need std.io.{Print}
need std.list.{Any, Count, Fold, Keep, ListNew, MapInPlace}

fun main() uses io, unsafe {
  var list = ListNew<int>()
  var i = 1
  while i <= 6 {
    list.Push(i)
    i = i + 1
  }
  const items = list.Items()      // a view, not a copy
  const limit = 3
  Print("{Any(items, |x| x > limit)}")              // true
  Print("{Count(items, |x| x % 2 == 0)}")           // 3
  Print("{Fold(items, 0, |total, x| total + x)}")   // 21
  MapInPlace(items, |x| x * 10)
  const kept = Keep(items, |x| x > 30)              // moves them to the front
  Print("{kept} left")
}
```

A closure captures by copy, so `limit` travels with it.

`Items()` is a view into the list's storage, valid only until the list next
grows (`Push`, `Reserve`) or dies. Read it after the last mutation; a view
kept across a `Push` reads freed memory (D227).

## std.map

A map needs to know how to hash a key and how to compare two:

```wolang
need std.hash.{TextEquals, TextHash}
need std.io.{Print}
need std.map.{MapNew}

fun main() uses io, unsafe {
  var counts = MapNew<text, int>(TextHash, TextEquals)
  counts.Put("wolf", 1)
  // `Get` takes what to answer when the key is not there, so a lookup
  // is one call rather than a lookup and a test.
  counts.Put("wolf", counts.Get("wolf", 0) + 1)
  Print("{counts.Get("wolf", 0)}")      // 2
  Print("{counts.Has("other")}")        // false
  Print("{counts.Len()} keys")
}
```

`std.map.IntHash`/`IntEquals` are there for integer keys.

## std.set

The same, holding keys only: `SetNew<T>(hash, equals)`, then `Add`, `Has`, `Remove`, `Len` on it.

## std.deque

Pushing and popping at both ends, which is what a queue and a stack both
want: `DequeNew<T>()`, then `PushBack`, `PushFront`, `PopBack`,
`PopFront`, `At`, `Len`.

## std.heap

A binary heap, for "what is the smallest thing left":
`HeapNew<T>(before)`, then `Push`, `Pop`, `Peek`, `Len`.

## std.sort

```wolang
need std.io.{Print}
need std.list.{ListNew}
need std.sort.{Contains, LowerBound, Sort}

fun main() uses io, unsafe {
  var list = ListNew<int>()
  list.Push(5)
  list.Push(1)
  list.Push(4)
  const items = list.Items()
  // The order is a closure, so the same sort works for anything.
  Sort(items, |a, b| a < b)
  Print("{items[0]} {items[1]} {items[2]}")        // 1 4 5
  Print("{LowerBound(items, 4, |a, b| a < b)}")    // 1
  Print("{Contains(items, 9, |a, b| a < b)}")      // false
}
```

## std.doc

One value type for every format: a nothing, a truth, a number, some
text, a list and a table.

```wolang
need std.doc.{AsInt, AsText, Get, Null, Object, OfInt, OfText, Put}
need std.io.{Print}

fun main() uses io, unsafe {
  var server = Object()
  server = Put(server, "host", OfText("example.com"))
  server = Put(server, "port", OfInt(443))
  var root = Object()
  root = Put(root, "server", server)

  // One path syntax for every format: names walk into tables, numbers
  // walk into lists.
  Print(AsText(Get(root, "server.host") ?? Null(), ""))
  Print("{AsInt(Get(root, "server.port") ?? Null(), 0)}")
}
```

`Kind`, `Member`, `At`, `Len`, `NameAt`, `ValueAt`, `AsNumber`,
`AsBool`, `IsNull`, `Array`, `Push`, `Number`, `OfBool`.

## std.json

```wolang
need std.doc.{AsText, Member, Null}
need std.io.{Print}
need std.json.{Parse, Write}

fun main() uses io, unsafe {
  match Parse("\{\"name\": \"wolang\", \"stars\": 42\}") {
    Ok(value) => {
      Print(AsText(Member(value, "name") ?? Null(), ""))
      Print(Write(value))
    },
    Malformed => { Print("not json") },
  }
}
```

Arrays and objects nested more than 128 deep answer `Malformed` rather than
exhausting the stack (D227).

## std.toml

TOML 1.0 as it is used: strings in four styles, numbers in four bases,
booleans, dates as text, arrays, inline tables, dotted keys, `[table]`
and `[[array of tables]]`. Arrays and inline tables nested deeper than 64
answer `Malformed` instead of exhausting the stack (D227). `Malformed` carries
the `line` and `column` the document went wrong at, counted from one, in `json`,
`toml` and `yaml` (D235): `Malformed(line, column) => ..`.

```wolang
need std.doc.{AsText, Get, Null}
need std.io.{Print}
need std.toml.{Parse}

fun main() uses io, unsafe {
  const source = "name = \"wolang\"\n\n[package]\nsource = \"src\"\n"
  match Parse(source) {
    Ok(value) => {
      Print(AsText(Get(value, "package.source") ?? Null(), ""))
    },
    Malformed => { Print("not toml") },
  }
}
```

## std.yaml

YAML 1.2: block and flow collections, every scalar style, block scalars
with chomping, anchors and aliases, merge keys, the standard tags, and
documents in a stream (`ParseAll`). Block or flow nesting deeper than 64
answers `Malformed` instead of exhausting the stack (D227).

```wolang
need std.doc.{AsInt, Get, Null}
need std.io.{Print}
need std.yaml.{Parse}

fun main() uses io, unsafe {
  const source = "base: &base\n  port: 80\nlive:\n  <<: *base\n" +
                 "  port: 443\n"
  match Parse(source) {
    Ok(value) => {
      Print("{AsInt(Get(value, "live.port") ?? Null(), 0)}")   // 443
    },
    Malformed => { Print("not yaml") },
  }
}
```

## std.ini

`[section]`, `name = value`, `;` and `#` comments. Every value is text,
because INI has no types and a reader that guessed would turn
`version = 1.10` into `1.1`.

## std.csv

RFC 4180: quoted fields, doubled quotes, line breaks inside quotes, and
a separator you choose.

```wolang
need std.csv.{Named, Read, Write}
need std.doc.{AsText, Member, Null}
need std.io.{Print}

fun main() uses io, unsafe {
  const rows = Read("name,price\nbread,2\n")
  Print(rows.At(1).At(0))                     // bread
  const tables = Named("name,price\nbread,2\n", 44)
  Print(AsText(Member(tables.At(0), "price") ?? Null(), ""))
}
```

## std.io

```wolang
need std.io.{OutNew, Print, ReadLine, Warn}

fun main() uses io, unsafe {
  Print("to standard output")
  Warn("to standard error")
  if const line = ReadLine() {
    Print("you said {line}")
  }
  // Buffered, for a program that writes a great deal.
  var out = OutNew()
  var i = 0
  while i < 1000 {
    out.Write("line {i}\n")
    i = i + 1
  }
  out.Flush()
}
```

## std.fs

```wolang
need std.fs.{Append, Copy, Exists, MakeDirectory, ReadText, RemoveTree,
             TempName, Walk, WriteText}
need std.io.{Print}

fun main() uses io, unsafe {
  const root = TempName("demo")
  const _made = MakeDirectory(root + "/inner", true)
  const _wrote = WriteText(root + "/inner/one.txt", "hello")
  const _added = Append(root + "/inner/one.txt", " again")
  Print(ReadText(root + "/inner/one.txt", 4096))   // hello again
  const _copied = Copy(root + "/inner/one.txt", root + "/two.txt", true)
  Print(Walk(root))                                 // every path under it
  const _gone = RemoveTree(root)
  Print("{Exists(root)}")                           // false
}
```

Also `Open`/`Create` for a kept-open file, `KindOf`, `SizeOf`,
`ChangedAt`, `Rename`, `List`, `Current`, `SetCurrent`, `Home`,
`TempDirectory`, `Absolute`.

## std.path

`Join`, `Parent`, `Name`, `Stem`, `Extension`, `WithExtension`,
`IsAbsolute`, `Normalize`, `IsInside` — string work, no filesystem.
`Normalize` answers `""` for a path with more than 64 parts left after
`.` and `..` are worked out; `IsInside` is false for such a path.

## std.net

```wolang
need std.io.{Print}
need std.net.{Accept, Connect, Listen, Resolve}

fun main() uses io, net, unsafe {
  if const address = Resolve("example.com") {
    Print("resolved to {address}")
  }
  var socket = Connect("example.com", 80)
  if socket.IsOpen() {
    const _sent = socket.Send("GET / HTTP/1.0\r\n\r\n")
    Print(socket.Receive(256))
    socket.Close()
  }
}
```

UDP, for game networking (D244). A datagram is one message: it arrives whole
or not at all, and a buffer that is too small cuts it and sets `truncated`.

```wolang
need std.io.{Print}
need std.mem.{ArrayNew, Free}
need std.net.{BindUdpOn, ParseAddress}

fun main() uses io, net, unsafe {
  const lo = ParseAddress("127.0.0.1") ?? 0
  var server = BindUdpOn(lo, 0)          // port 0: the system picks
  var client = BindUdpOn(lo, 0)
  const _sent = client.SendTextTo(lo, server.LocalPort(), "ping")
  var buffer = ArrayNew<u8>(512)
  if const got = server.ReceiveFrom(buffer) {
    Print("{got.count} bytes from port {got.port}, cut: {got.truncated}")
  }
  Free(buffer)
  server.SetNonBlocking(true)            // ReceiveFrom now answers null
  client.Close()                         // when nothing is waiting
  server.Close()
}
```

`BindUdp(port)` binds every interface; `SetBroadcast(true)` allows sending
to the broadcast address. `SendTo` takes `[]u8`, answers the bytes sent or
-1 (closed, empty, or over 65507 bytes). `UdpSocket.fd` can be watched by
a poller like a TCP descriptor.

## std.process

```wolang
need std.io.{Print}
need std.process.{CommandNew, Start, StartCommand, StartWith,
                  WithDirectory, WithVariable}

fun main() uses io, unsafe {
  const said = Start("git rev-parse HEAD")
  Print("{said.code}: {said.out}")

  // Something written to its input.
  Print(StartWith("wc -l", "a\nb\nc\n").out)

  // Somewhere to run, and something in its environment.
  var run = CommandNew("sh -c 'pwd; echo $MARK'")
  run = WithDirectory(run, "/tmp")
  run = WithVariable(run, "MARK", "seen")
  Print(StartCommand(run).out)
}
```

No shell: the command is split on blanks with quoted runs kept whole, and
the program is started directly.

## std.env

```wolang
need std.env.{ArgAt, ArgCount, Get, Lookup, Set}
need std.io.{Print}

fun main() uses io, unsafe {
  Print("{ArgCount()} arguments; the first is {ArgAt(0)}")
  Print(Get("HOME", "nowhere"))
  if const held = Lookup("EDITOR") {
    Print("editor is {held}")
  }
  Set("WOLANG_RAN", "1")
}
```

## std.time

```wolang
need std.io.{Print}
need std.time.{Ago, Civil, CivilIn, Elapsed, Iso, IsoInstant, Now,
               ParseIso, WallClock, ZoneOffset}

fun main() uses io, time, unsafe {
  const started = Now()
  const moment = WallClock().seconds
  Print(IsoInstant(moment))                     // 2026-10-15T18:09:23Z
  Print(Iso(CivilIn(moment, "Europe/Kyiv")))    // the same moment, there
  Print("{ZoneOffset(moment, "Europe/Kyiv") / 3600} hours ahead")
  if const parsed = ParseIso("2026-10-15T18:09:23+02:00") {
    Print(Ago(parsed, moment))                  // "3 hours ago"
  }
  Print("{Elapsed(started, Now())} nanoseconds in this program")
}
```

## std.crypto

```wolang
need std.crypto.{Hex, Hmac, SameDigest, Sha256Of, Token, Uuid, UuidAt}
need std.io.{Print}
need std.text.{Bytes}

fun main() uses io, rand, unsafe {
  Print(Hex(Sha256Of("abc")))        // ba7816bf...
  const signature = Hmac(Bytes("key"), Bytes("message"))
  Print("{SameDigest(signature, signature)}")
  Print(Uuid())                      // random, version 4
  Print(UuidAt(1792087763000))       // sortable, version 7
  Print(Token(16))                   // 32 hexadecimal characters
}
```

Randomness comes from the system, not from `std.random`: a token made
from a sequence is a token someone can continue.

## std.compress

```wolang
need std.compress.{Crc32, Gunzip, Gzip}
need std.io.{Print}
need std.text.{Bytes}

fun main() uses io, unsafe {
  const packed = Gzip(Bytes("hello hello hello hello"))
  Print("{packed.len} bytes")
  match Gunzip(packed) {
    Ok(back) => { Print("{back.len} back") },
    Malformed => { Print("not a stream") },
  }
  Print("{Crc32(Bytes("123456789"))}")   // 3421780262
}
```

What it writes, the system's `gzip` reads, and the other way round.

`Inflate` and `Gunzip` put no cap on the output, so a hostile stream can
expand about a thousandfold. For anything that arrived from outside use
`InflateLimited(bytes, most)` / `GunzipLimited(bytes, most)`: they answer
`Malformed` once the output would pass `most` bytes (D227).

## std.encoding

`ToBase64`, `FromBase64`, `ToHex`, `FromHex`, `ToUrl`, `FromUrl`.

## std.hash

`OfBytes`, `OfText`, `OfInt`, `Combine`, `Slot`, `TextHash`,
`TextEquals` — fast hashing for tables, not for signatures.

## std.random

```wolang
need std.io.{Print}
need std.random.{RandomFromClock, RandomNew}

fun main() uses io, time, unsafe {
  var rng = RandomNew(42)     // the same seed is the same sequence
  Print("{rng.Next() % 6 + 1}")
  Print("{rng.Between(1, 100)}")
  var other = RandomFromClock()
  Print("{other.Next()}")
}
```

## std.log

```wolang
need std.log.{Debug, Error, Info, Level, SetLevel, SetTimestamps,
              Trace, Warning}

fun main() uses io, time, unsafe {
  SetLevel(Level.Debug)
  SetTimestamps(true)
  Trace("the finest level, usually off")
  Debug("starting")
  Info("listening on 8080")
  Warning("slow request")
  Error("gave up")
}
```

## std.thread

```wolang
need std.io.{Print}
need std.thread.{AtomicAdd, Parallelism, Spawn}

var total: s64 = 0

fun Work() uses unsafe {
  var i = 0
  while i < 1000 {
    const _held = AtomicAdd(&mut total, 1)
    i = i + 1
  }
}

fun main() uses io, unsafe {
  Print("{Parallelism()} cores")
  var worker = Spawn(Work)
  Work()
  const _joined = worker.Join()
  Print("{total}")        // 2000
}
```

A body started on another thread (`Spawn`, `ParallelFor`: the
parameters of functions marked `@Thread`) may not read or write a
module-level `var`, directly or through the functions and methods it
calls, in any module, unless the function making the call states
`uses unsafe` (`E0147`); a function that states it stops the walk
(D252), and `Spawn`
takes no capturing closure at all (`E0148`); `ParallelFor`'s closure
may capture, each thread getting a copy (D232).

## std.tasks

`std.thread.ParallelFor(count, body)` is a range split across the cores,
which is what a frame's worth of work wants. `std.tasks` is the
cooperative side: `Run`, `Done`, `Pending` and `Join` for `spawn`ed
tasks.

## std.mem

`ArrayNew`, `Free`, `Prefix`, `Suffix`, `BytesFromRaw`, `ViewFromRaw`,
`SizeOf`, `SoaNew`. Storage and views; the language's arrays and lists
are built on these.

## std.own

`Owned<T>` and `Shared<T>`: a value that frees what it holds, and one
that counts how many hold it.

---

## std.ops

`Add`, `Sub`, `Mul`, `Div`, `Neg`, `Eq`, `Ord`, `Hash`, `Max` and `Min`.
Each of the first seven is an `@Operator` ability whose one method is what
an operator calls (D239): `Plus` for `+`, `Minus`, `Times`, `Divide`,
`Negate` for unary `-`, `Equals` for `==` and `!=`, and `Compare` for
`< <= > >=`. `Self` is the type that has the ability.

```wolang
need std.io.{Print}
need std.ops.{Eq, Max, Ord}

struct Level {
  var n: int
}

extend Level as Eq {
  fun Equals(other: &Self) -> bool => n == other.n
}

extend Level as Ord {
  fun Compare(other: &Self) -> int => n - other.n
}

fun main() uses io {
  const top = Max(Level(3), Level(8))
  Print("{top.n} {Level(1) == Level(1)} {Level(1) < Level(2)} {Max(3, 9)}")
}
```

Numbers, `bool`, `char` and `text` have the operators they were always
given, so `Max(3, 9)` is a compare and not a call. `Hash` is not an
operator: it is the number `std.map` files a key under.

## The `http` package

A server and a client. See [the cookbook](cookbook.md#serve-an-api) for
longer examples.

```wolang
need http.{AppNew, Fetch, Ok, Request, Response, ServeWithDefaults}

fun Health(_request: Request) -> Response => Ok("ok\n")

fun main() uses io, net, time, unsafe {
  var app = AppNew()
  app.Get("/health", Health)
  const _code = ServeWithDefaults(app, 8080, 512)
}
```

Routing with path parameters and nested routers, middleware, typed
extractors, static files with the traversal checks, and a client with
TLS, redirects, chunked bodies and timeouts.

## The `db` package

A key-value store on a disk that survives the power going out.

```wolang
need db.{Close, Open}
need std.io.{Print}

fun main() uses io, unsafe {
  match Open("state.db") {
    Ok(store) => {
      var held = store
      const _put = held.Put("user:1", "a value")
      Print(held.Get("user:1") ?? "nothing")
      const keys = held.Range("user:", "user;")
      var i = 0
      while i < keys.Len() {
        Print(keys.At(i))
        i = i + 1
      }
      Close(&mut held)
    },
    Unreadable => { Print("could not open") },
  }
}
```

## The `gfx` package

A window, input, pixels, OpenGL and sound, over this project's own
backend for each system.

```wolang
need gfx.{Available, CanvasNew, Open, Rgb, kClosed, kResizable}

fun main() uses io, unsafe {
  if !Available() {
    return
  }
  var window = Open("demo", 640, 480, kResizable)
  var canvas = CanvasNew(640, 480)
  var running = true
  while running {
    loop {
      const event = window.Poll()
      if event.kind == 0 {
        break
      }
      if event.kind == kClosed {
        running = false
      }
    }
    canvas.Clear(Rgb(16, 16, 24))
    canvas.Rect(100, 100, 80, 80, Rgb(255, 200, 60))
    window.Present(&canvas)
  }
}
```

Beyond the window itself (D247), all of it answers "no" cleanly on a build
with no backend, so a headless run needs no guards:

| Area | API | Notes |
|---|---|---|
| Cursor | `ShowCursor(bool)`, `SetCursor(kCursorArrow/Hand/Text/Crosshair/ResizeH/ResizeV/ResizeAll)`, `LockCursor(bool) -> bool` | locked: `kMouseMoved` carries the movement in `scroll_x`/`scroll_y` (unlocked it does too). X11 grabs and warps, Win32 clips and reads raw input, Cocoa detaches the cursor |
| Window | `SetFullscreen`, `IsFullscreen`, `SetSize`, `SetMinSize`, `SetPosition`, `X()`, `Y()` | content sizes in pixels. Cocoa uses the system full screen (its own Space) |
| Events | `kDropped` (`key` = count, `DropPath(i)`), `kScaleChanged` (`scroll_x` = new scale) | X11 reads `Xft.dpi`; drops use XDND with `text/uri-list` |
| Gamepads | `GamepadConnected(i)`, `GamepadName`, `GamepadButton(i, kPadA..)`, `GamepadAxis(i, kAxisLeftX..)`, events `kGamepadConnected` / `kGamepadDisconnected` | up to 4, one layout: sticks -1..1 with up and left negative, triggers 0..1, no deadzone applied. `Deadzone(v, dead)` and `AxisNormalize(raw, low, high)` are helpers. Linux reads `/dev/input/event*` (needs the `input` group, scanned once a second), Windows XInput, macOS GameController |
| Dialogs | `MessageBox(title, body, kDialogInfo/Warning/Error, kButtonsOk/OkCancel/YesNo) -> kAnswer*`, `OpenFileDialog`, `SaveFileDialog` (both `text?`), `DialogsAvailable()` | Linux runs `zenity` or `kdialog` and otherwise answers `kAnswerUnavailable` / null |

Pad events arrive through `Window.Poll` after the window's own.

---

## The `image` package

PNG, QOI and BMP, read and written in Wolang (D245). Everything decodes to an
`Image`: `width`, `height` and RGBA8 `pixels`, which it frees when it goes
out of scope. `Get(x, y)` answers `0xAARRGGBB`, the format a `gfx` canvas
holds, so putting an image on screen is a loop over `canvas.pixels` and this
package does not depend on `gfx`.

```wolang
need image.{Image, ImageNew, PngDecode, PngEncode, QoiEncode}
need std.io.{Print}

fun main() uses io, unsafe {
  match ImageNew(2, 2) {
    Ok(picture) => {
      picture.Set(0, 0, 0xffff0000)
      const file = PngEncode(&picture)
      match PngDecode(file) {
        Ok(back) => { Print("{back.width}x{back.height} {back.Get(0, 0)}") },
        _ => { Print("unreadable") },
      }
      Print("{QoiEncode(&picture).len} bytes of QOI")
    },
    _ => { Print("too large") },
  }
}
```

| Function | What |
|---|---|
| `PngDecode(bytes)` | every colour type, depths 1 to 16 (16 is cut to 8), all filters, Adam7, `PLTE`, `tRNS`; checks CRCs, zlib header and Adler-32 |
| `PngEncode(&image)` | RGBA8, best of the five filters per row, std deflate |
| `QoiDecode` / `QoiEncode` | the whole format |
| `BmpDecode` | 24 and 32 bit, uncompressed, bottom-up or top-down |
| `BmpEncode` | 32 bit, bottom-up |
| `ImageNew(w, h)` | a transparent picture |

Decoders answer `Malformed` (bad signature, checksum, length, or data that
stops early), `Unsupported` (a real feature not done: compressed or palette
BMP, an unknown critical PNG chunk) or `TooLarge` (over `kMaxSide` = 16384
on a side or `kMaxPixels` = 64 Mi pixels); sizes are checked before any
allocation and the PNG inflater is limited to what the header implies
(D227), so a hostile file cannot trap or balloon. Encoders return storage
the caller owns.

---

## The `math` package

Vectors, matrices and quaternions for gameplay and rendering code, written in
Wolang (D263). `Vec2`, `Vec3` and `Vec4` are structs of `float` (`f32`, like
`std.math`), and the arithmetic is `std.ops` abilities (D239): `a + b`, `a - b`,
`-a`, `a * b`, `a / b` and `a == b` work on all of them, component by
component. `Mat3`, `Mat4` and `Quat` meet `Mul` (a matrix or Hamilton product,
the right side applied first) and `Eq`. Equality is exact; use `NearTo` for a
tolerance. Anything that takes a square root states `uses unsafe`, as
`std.math.Sqrt` does.

```wolang
need math.{Vec3, Mat4Translation, QuatAxisAngle}
need std.io.{Print}

fun main() uses io, unsafe {
  const turn = QuatAxisAngle(Vec3(0.0, 0.0, 1.0), 1.5707963)
  const move = Mat4Translation(Vec3(10.0, 0.0, 0.0))
  const point = move.TransformPoint(turn.Rotate(Vec3(1.0, 0.0, 0.0)))
  Print("{point.x} {point.y} {point.z}")
  const sum = Vec3(1.0, 2.0, 3.0) + Vec3(1.0, 1.0, 1.0)
  const length = sum.Length()
  Print("{length}")
}
```

| Function | What |
|---|---|
| `Dot(other)`, `LengthSquared()`, `Length()`, `Distance(other)` | on `Vec2`, `Vec3`, `Vec4` (`Distance` not on `Vec4`); `Quat` has `Dot` and `Length` |
| `Cross(other)` | `Vec3` only |
| `Normalized()` | unit length; the zero vector stays zero (a zero `Quat` gives the identity) |
| `Scaled(k)`, `Divided(k)` | by a number; `*` and `/` between vectors are per component |
| `Lerped(other, t)`, `Clamped(low, high)`, `NearTo(other, tolerance)` | per component (`Clamped` on `Vec2`, `Vec3`) |
| `Mat3Identity()`, `Mat4Identity()` | |
| `Mat4Translation(by)`, `Mat4Scale(by)`, `Mat4FromMat3(m)` | |
| `Transform(v)` | `Mat3` times a `Vec3` |
| `TransformPoint(p)`, `TransformDirection(d)`, `TransformVec4(v)` | `Mat4`; a point gets the translation, a direction does not |
| `Transposed()`, `Determinant()`, `Upper()` | `Determinant` is `Mat3`, `Upper` takes the `Mat3` out of a `Mat4` |
| `QuatIdentity()`, `QuatAxisAngle(axis, radians)` | the axis need not be a unit vector |
| `Rotate(v)`, `Conjugate()`, `Slerp(other, t)`, `ToMat3()`, `ToMat4()` | `Quat`; `Slerp` takes the short way round |

There is no general matrix inverse and no projection matrix yet.

## How this compares

Go's standard library is bigger — net/http with HTTP/2, crypto/tls,
database/sql, encoding/xml, reflection, a race detector. Rust's is
smaller than this on purpose and leans on crates. Zig's is close to this
in size and shape.

What is here that most of those have: strings, collections, sorting,
files, paths, processes, sockets, time with zones, JSON, hashing,
randomness, threads, logging. What is here that several of them leave to
a package manager: TOML, YAML, INI, CSV, regular expressions, deflate and
gzip, an HTTP server *and* client, a key-value store, a window with a GPU
context and sound.

What is not here: a TLS implementation (bound, not written — `lib/tls`),
HTTP/2 and HTTP/3, a SQL database, XML, reflection, a garbage collector
that does anything, big integers, and a date library as thorough as
`java.time`.
