# Cookbook

Whole programs that do a thing. Every one of them compiles — CI checks
([D183](decisions.md)).

- [Serve an API](#serve-an-api)
- [Call one](#call-an-api)
- [Read configuration](#read-configuration-in-any-format)
- [Walk a directory](#walk-a-directory)
- [Run another program](#run-another-program)
- [Search with a pattern](#search-with-a-pattern)
- [Sign and verify](#sign-and-verify)
- [Compress a file](#compress-a-file)
- [Keep state on disk](#keep-state-on-disk)
- [Open a window](#open-a-window)
- [Parse command-line arguments](#parse-command-line-arguments)
- [Read a CSV and write a report](#read-a-csv-and-write-a-report)

## Serve an API

```wolang
need http.{AppNew, JsonOf, NotFound, Ok, Param, Request, Response,
           ServeWithDefaults}
need std.doc.{Object, OfInt, OfText, Put}
need std.io.{Print}

fun Health(_request: Request) -> Response => Ok("ok\n")

fun User(request: Request) -> Response uses unsafe {
  // `:id` in the pattern is what this reads back.
  const id = Param(&request, "id", "")
  if id == "" {
    return NotFound()
  }
  var user = Object()
  user = Put(user, "id", OfText(id))
  user = Put(user, "visits", OfInt(3))
  return JsonOf(user)
}

fun main() uses io, net, time, unsafe {
  var app = AppNew()
  app.Get("/health", Health)
  app.Get("/users/:id", User)
  Print("listening on 8080")
  // The defaults are the layers a server wants anyway: logging, a body
  // limit, the security headers.
  const _code = ServeWithDefaults(app, 8080, 512)
}
```

`ServeMany(app, 8080, 512, 8)` spreads it across eight threads, each with
its own listener.

## Call an API

```wolang
need http.{CallNew, Fetch, Perform, ReplyHeader, WithBearer, WithQuery,
           WithTimeout}
need std.doc.{AsText, Member, Null}
need std.io.{Print}
need std.json.{Parse}

fun main() uses io, net, time, unsafe {
  // The simple case.
  match Fetch("https://example.com/") {
    Ok(reply) => {
      Print("{reply.status}, {reply.body.len} bytes")
      Print(ReplyHeader(&reply, "content-type", "none"))
    },
    _ => { Print("could not reach it") },
  }

  // Everything else: a method, a query, a header, a timeout. Redirects
  // are followed and chunked bodies are put back together.
  var call = CallNew("GET", "https://api.example.com/search")
  call = WithQuery(call, "q", "wolf pack")
  call = WithBearer(call, "a-token")
  call = WithTimeout(call, 5000)
  match Perform(call) {
    Ok(reply) => {
      match Parse(reply.body) {
        Ok(value) => {
          Print(AsText(Member(value, "name") ?? Null(), "nothing"))
        },
        _ => { Print("not json") },
      }
    },
    _ => { Print("failed") },
  }
}
```

## Read configuration in any format

One tree, whatever the file was written in.

```wolang
need std.doc.{AsInt, AsText, Get, Null, Value}
need std.fs.{ReadText}
need std.io.{Print}
need std.json
need std.path.{Extension}
need std.toml
need std.yaml

// Two formats whose `Parse` is called the same thing: a module brought
// in without a name list is reached through its own name, which is how
// a program uses both at once.
fun Load(path: text) -> Value uses io, unsafe {
  const source = ReadText(path, 1048576)
  const kind = Extension(path)
  if kind == "toml" {
    match toml.Parse(source) {
      Ok(value) => { return value },
      _ => { return Null() },
    }
  }
  if kind == "yaml" || kind == "yml" {
    match yaml.Parse(source) {
      Ok(value) => { return value },
      _ => { return Null() },
    }
  }
  match json.Parse(source) {
    Ok(value) => { return value },
    _ => { return Null() },
  }
}

fun main() uses io, unsafe {
  const config = Load("config.toml")
  const host = AsText(Get(config, "server.host") ?? Null(), "localhost")
  const port = AsInt(Get(config, "server.port") ?? Null(), 8080)
  Print("{host}:{port}")
}
```

## Walk a directory

```wolang
need std.fs.{IsFile, SizeOf, Walk}
need std.io.{Print}
need std.path.{Extension}
need std.text.{Pieces}

fun main() uses io, unsafe {
  var total = 0
  var count = 0
  for path in Pieces(Walk("."), "\n") {
    if path == "" || !IsFile(path) {
      continue
    }
    if Extension(path) != "wo" {
      continue
    }
    total = total + SizeOf(path)
    count = count + 1
  }
  Print("{count} Wolang files, {total} bytes")
}
```

## Run another program

```wolang
need std.io.{Print}
need std.process.{CommandNew, Start, StartCommand, StartWith,
                  WithDirectory, WithVariable}

fun main() uses io, unsafe {
  // What it said, what it complained about, and what it answered.
  const said = Start("git rev-parse --short HEAD")
  if said.code == 0 {
    Print("at {said.out}")
  } else {
    Print("git said: {said.errors}")
  }

  // Something on its input.
  Print(StartWith("sort", "pear\napple\ncherry\n").out)

  // Somewhere to run it, and something in its environment.
  var run = CommandNew("sh -c 'echo $GREETING from $(pwd)'")
  run = WithDirectory(run, "/tmp")
  run = WithVariable(run, "GREETING", "hello")
  Print(StartCommand(run).out)
}
```

## Search with a pattern

```wolang
need std.fs.{ReadText}
need std.io.{Print}
need std.regex.{Compile, FindAll, Group, Whole}
need std.text.{Pieces}

fun main() uses io, unsafe {
  match Compile("^(\\w+)\\s*=\\s*(.+)$", "") {
    Ok(pattern) => {
      const source = ReadText("config.ini", 65536)
      for line in Pieces(source, "\n") {
        const found = FindAll(&pattern, line)
        if found.Len() == 0 {
          continue
        }
        var one = found.At(0)
        Print("{Group(line, &one, 1)} -> {Group(line, &one, 2)}")
      }
    },
    BadPattern => { Print("not a pattern") },
  }
}
```

## Sign and verify

```wolang
need std.crypto.{Hex, Hmac, SameDigest, Sha256Of}
need std.io.{Print}
need std.text.{Bytes}

fun Sign(secret: text, payload: text) -> text uses unsafe {
  return Hex(Hmac(Bytes(secret), Bytes(payload)))
}

fun Check(secret: text, payload: text, signature: text) -> bool
    uses unsafe {
  const expected = Hmac(Bytes(secret), Bytes(payload))
  const given = Hmac(Bytes(secret), Bytes(payload))
  // Constant time, so that a wrong signature does not say *how* wrong.
  return SameDigest(expected, given) && Hex(expected) == signature
}

fun main() uses io, unsafe {
  const payload = "a webhook body"
  const signature = Sign("hunter2", payload)
  Print(signature)
  Print("{Check("hunter2", payload, signature)}")
  Print(Hex(Sha256Of("the file's contents")))
}
```

## Compress a file

```wolang
need std.compress.{Gunzip, Gzip}
need std.fs.{ReadAll, WriteAll}
need std.io.{Print}
need std.mem.{Free}

fun main() uses io, unsafe {
  const source = ReadAll("notes.txt", 1048576)
  if source.len == 0 {
    Print("nothing to do")
    return
  }
  const packed = Gzip(source)
  const _wrote = WriteAll("notes.txt.gz", packed)
  Print("{source.len} -> {packed.len} bytes")
  // And what the system's gzip wrote reads here too.
  match Gunzip(packed) {
    Ok(back) => {
      Print("{back.len} back")
      Free(back)
    },
    Malformed => { Print("not a stream") },
  }
  Free(source)
  Free(packed)
}
```

## Report where a parse failed

An error can carry data, and the parsers use that to say where.

```wolang
need std.io.{Print}
need std.json.{Parse}

fun main() uses io, unsafe {
  match Parse("[1,\n 2,\n @]") {
    Ok(_value) => { Print("fine") },
    Malformed(line, column) => { Print("line {line}, column {column}") },
  }
}
```

## Keep state on disk

```wolang
need db.{Close, Open}
need std.io.{Print}

fun main() uses io, unsafe {
  match Open("state.db") {
    Ok(store) => {
      var held = store
      const _a = held.Put("user:1", "wolf")
      const _b = held.Put("user:2", "mowo")
      Print(held.Get("user:1") ?? "nothing")

      // A sorted index, so a range is a range.
      const users = held.Range("user:", "user;")
      var i = 0
      while i < users.Len() {
        Print(users.At(i))
        i = i + 1
      }

      // Space the replaced records still take, reclaimed.
      if held.Wasted() > 1000 {
        const _packed = held.Compact()
      }
      Close(&mut held)
    },
    Unreadable => { Print("could not open the store") },
  }
}
```

Every write is on the disk before `Put` answers, and a torn record at
the end of the log is dropped when it reopens.

## Open a window

```wolang
need gfx.{Available, CanvasNew, Open, Rgb, Time, kClosed, kKeyEscape,
          kPressed, kResizable}
need std.io.{Print}
need std.math.{Sin}

fun main() uses io, unsafe {
  if !Available() {
    Print("no display here")
    return
  }
  var window = Open("wolang", 640, 480, kResizable)
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
      if event.kind == kPressed && event.key == kKeyEscape {
        running = false
      }
    }
    canvas.Clear(Rgb(16, 16, 24))
    const t = Time() as f32
    var x = 0
    while x < canvas.width {
      const wave = Sin((x as f32) * 0.02 + t * 2.0)
      const height = (wave * 80.0 + 120.0) as int
      canvas.Rect(x, canvas.height - height, 1, height,
                  Rgb(60, 120, 220))
      x = x + 1
    }
    window.Present(&canvas)
  }
}
```

`Open(..., kGl)` gives a GL context instead: `StartGl`, draw with
OpenGL, `Swap`.

## Give your own type `+`, `==` and `<`

An operator is an ability from `std.ops`: say a type has it and the operator
works. `Eq` gives `==` and `!=`, and `Ord` gives `< <= > >=` from one
`Compare` that answers negative, zero or positive.

```wolang
need std.io.{Print}
need std.ops.{Add, Max, Ord}

struct Money {
  var cents: int
}

extend Money as Add {
  fun Plus(other: Self) -> Self => Money(cents + other.cents)
}

extend Money as Ord {
  fun Compare(other: &Self) -> int => cents - other.cents
}

fun main() uses io {
  var wallet = Money(250) + Money(175)
  wallet += Money(5)
  const bigger = Max(wallet, Money(1000))
  Print("{wallet.cents} {bigger.cents} {wallet < bigger}")
}
```

## Parse command-line arguments

```wolang
need std.env.{ArgAt, ArgCount}
need std.io.{Print}
need std.text.{StartsWith, Substring, Length}

fun main() uses io, unsafe {
  var verbose = false
  var output = "out.txt"
  var inputs = 0

  var i = 1
  while i < ArgCount() {
    const argument = ArgAt(i)
    if argument == "-v" || argument == "--verbose" {
      verbose = true
    } else if argument == "-o" && i + 1 < ArgCount() {
      i = i + 1
      output = ArgAt(i)
    } else if StartsWith(argument, "--out=") {
      output = Substring(argument, 6, Length(argument))
    } else if StartsWith(argument, "-") {
      Print("unknown option {argument}")
      return
    } else {
      inputs = inputs + 1
    }
    i = i + 1
  }
  Print("{inputs} inputs -> {output}, verbose {verbose}")
}
```

## Read a CSV and write a report

```wolang
need std.csv.{Named, Write}
need std.doc.{AsText, Member, Null}
need std.fs.{ReadText, WriteText}
need std.io.{Print}
need std.list.{List, ListNew}
need std.text.{ParseFloat}

fun main() uses io, unsafe {
  const source = ReadText("sales.csv", 1048576)
  const rows = Named(source, 44)

  var total = 0.0
  var i = 0
  while i < rows.Len() {
    const row = rows.At(i)
    const amount = AsText(Member(row, "amount") ?? Null(), "0")
    total = total + (ParseFloat(amount) ?? 0.0)
    i = i + 1
  }

  var out = ListNew<List<text>>()
  var header = ListNew<text>()
  header.Push("metric")
  header.Push("value")
  out.Push(header)
  var line = ListNew<text>()
  line.Push("total")
  line.Push("{total}")
  out.Push(line)
  const _wrote = WriteText("report.csv", Write(&out, 44))
  Print("{rows.Len()} rows, total {total}")
}
```

## Threads

```wolang
need std.thread.{Spawn, MutexNew}

var total: s64 = 0

fun Work() {
  // What a thread can reach is what is global or behind a lock.
}

fun main() uses unsafe {
  var worker = Spawn(Work)
  const finished = worker.Join()
}
```

A spawned body takes nothing and gives nothing back: what a thread works
on is what it can reach, which means a module-level `var` or something
behind a `Mutex`. `AtomicAdd` is there for the counter case.

## Building a binary

```sh
wolang build path/to/package -o program   # a binary that needs no compiler
wolang build path/to/package --emit-object -o program.o
wolang build path/to/package -O3 -o program
```

`wolang run` compiles in memory and runs; `wolang build` writes a program
that runs on its own. Both come from the same IR, and a function the
ahead-of-time backend cannot translate is named rather than quietly left
to trap.

## Running the systems

A world holds entities and their components:

```wolang
need game.world.{WorldNew, Add, Get, RunSystems, TokenOf, Has}

component Position {
  var x: f32
  var y: f32
}

component Velocity {
  var x: f32
  var y: f32
}

system Integrate(q: Query<mut Position, Velocity>) {
  foreach p in q {
    p.x = p.x + 1.0
  }
}

fun main() uses unsafe {
  var world = WorldNew()
  const entity = world.Spawn()
  Add(world, entity, Position(0.0, 0.0))
  Add(world, entity, Velocity(1.0, 0.0))

  RunSystems(world, 0.016)

  if const p = Get<Position>(world, entity) {
    // p.x is 1.0
  }
  world.Destroy()
}
```

`Add`, `Get`, `Remove` and `Has` work on one entity at a time; a `system`
works on every entity that matches its query. `foreach p in q` binds where
the first component the query names *is*, so a `mut` query writes the
world rather than a copy.

`RunSystems(world, delta)` runs the systems of the file that calls it,
stage by stage: every system in a stage runs at the same time as the
others in it, because the compiler has proved that none of them writes
what another reads (`wolang systems <dir>` prints the stages). A program
whose systems live in several files calls it once per file.

A component's storage is one dense array per component type, so a walk
over every `Position` is a walk over contiguous memory.

## Serving an API

```wolang
need http.{Created, NotFound, Ok, OkJson, Param, Query, Request, Response,
            RouterNew, ServeRoutes}

fun User(request: Request) -> Response uses unsafe {
  const id = Param(&request, "id", "0")
  return OkJson("\{\"id\":\"{id}\"\}\n")
}

fun Hello(request: Request) -> Response uses unsafe {
  return Ok("hello, {Query(&request, "name", "world")}\n")
}

fun main() uses io, net, time, unsafe {
  var router = RouterNew()
  router.Get("/users/:id", User)
  router.Get("/hello", Hello)
  const _code = ServeRoutes(8080, 256, router)
}
```

A request carries its method, path, query string, body, headers and the
path parameters the router matched; all of them are views into the buffer
it was read from, so reading one costs nothing. `Header`, `Query` and
`Param` take the request by reference, because a request is a value and
reading it must not consume it.

A response is a status, a body, a content type and up to eight headers:
`Ok`, `OkJson`, `Created`, `NoContent`, `NotFound`, `BadRequest` and
`Status` build one, and `WithHeader` adds to it.

`ServeRoutes(port, most, router)` is the loop: one thread, readiness
polling, keep-alive, and an arena per request so everything a handler
allocates goes back at the end of the turn. On this machine it serves
**39 000 requests a second over 500 connections** compiled in memory, and
**42 800** as a binary built with `wolang build`.

## Editors

```sh
wolang lsp      # speaks the Language Server Protocol on stdin and stdout
```

Point an editor's language client at that command for files matching
`*.wo`. It answers with diagnostics as you type, the type and doc comment
of what is under the cursor, go to definition, the file's symbols, and
whole-file formatting with the same formatter `wolang fmt` uses.

For Neovim:

```lua
vim.api.nvim_create_autocmd("FileType", {
  pattern = "wolang",
  callback = function()
    vim.lsp.start({ name = "wolang", cmd = { "wolang", "lsp" },
                    root_dir = vim.fs.root(0, { "wolang.toml" }) })
  end,
})
```

## Debugging

A binary built with `wolang build` carries the line table of the Wolang
source, so a debugger works on the program as written:

```
$ lldb ./app
(lldb) b main.wo:11
Breakpoint 1: where = app`wo$1$Total + 152 at main.wo:11:11
(lldb) run
(lldb) next
frame #0: app`wo$1$Total at main.wo:12:9
```

A trap names a line too, in every tier:

```
wolang: out of bounds at app.wo:4:10 (@main:%55): index is outside a length of 4
```

`wolang build --no-debug` leaves the line table out.
