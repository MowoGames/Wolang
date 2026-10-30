# The standard library

Thirty-four modules, all written in Wolang over a floor of about sixty C
calls. This is what each one is for, what it gives you, and what it does
not. Every example here compiles.

How to read a signature: `uses unsafe` on a standard library function
means *it* takes the responsibility, not you — `std.text.Bytes` looks at
a string's bytes, and a caller does not have to declare anything.

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
terminator; `Bytes` hands you the bytes and `TextFromRaw` takes them
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
answer `Malformed` instead of exhausting the stack (D227).

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
module-level `var`, directly or through the functions it calls, unless
the function making the call states `uses unsafe` (`E0147`), and `Spawn`
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

---

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
