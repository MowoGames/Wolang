# http

## http

### class `App`

```wolang
offer class App
```

Routes and the layers around them.

#### field `router`

```wolang
var router: Router
```

#### field `layers`

```wolang
var layers: Layers
```

#### field `limits`

```wolang
var limits: Limits
```

#### method `Get`

```wolang
fun Get(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

A route for `GET`. The same names a router has, so an application
can be built without reaching inside it.

#### method `Post`

```wolang
fun Post(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Put`

```wolang
fun Put(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Patch`

```wolang
fun Patch(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Delete`

```wolang
fun Delete(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Any`

```wolang
fun Any(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Otherwise`

```wolang
fun Otherwise(handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Mount`

```wolang
fun Mount(prefix: text, inner: &Router) uses unsafe
```

#### method `Use`

```wolang
fun Use(run: keep fun(&mut Request) -> Response?) uses unsafe
```

A layer that runs before every handler, and may answer instead.

#### method `Then`

```wolang
fun Then(run: keep fun(&Request, Response) -> Response) uses unsafe
```

A layer that runs after every handler.

### function `AppNew`

```wolang
offer fun AppNew() -> App uses unsafe
```

### function `ServeApp`

```wolang
offer fun ServeApp(app: App, port: int, most: int) -> s32 uses io, net, time, unsafe
```

Serves `app` on `port` until the program stops.

`Serve` is taken by the one-connection loop, so the application's
entry point says what it serves.

`most` connections at once, each with a buffer of its own.

### function `ServeWithDefaults`

```wolang
offer fun ServeWithDefaults(app: App, port: int, most: int) -> s32 uses io, net, time, unsafe
```

The same, with the usual layers already on: a line per request, the
security headers, and CORS.

What a program that has not decided anything yet should call, because
every one of those is something a server wants and none of them is
something a handler should have to remember.

### function `ServeMany`

```wolang
offer fun ServeMany(app: App, port: int, most: int, threads: int) -> s32 uses io, net, time, unsafe
```

Serves `app` on `port` from `threads` threads at once.

Zero threads means one per core. Every thread runs the same
application: a handler that keeps state in a module-level `var` is
sharing it across threads, and whether that is safe is the program's
business -- a counter wants `std.thread.AtomicAdd` and a table wants
a `Mutex` (D135).

### error `BadUrl`

```wolang
offer error BadUrl, NeedsTls, Unreachable, Truncated, NotHttp
```

### struct `Url`

```wolang
offer struct Url
```

A URL taken apart.

#### field `host`

```wolang
var host: text = ""
```

#### field `port`

```wolang
var port: int = 80
```

#### field `target`

```wolang
var target: text = "/"
```

The path and the query together, which is what a request line
carries.

#### field `secure`

```wolang
var secure: bool = false
```

### function `ParseUrl`

```wolang
offer fun ParseUrl(url: text) -> Url!BadUrl uses unsafe
```

`http://host:port/path?query` taken apart.

### struct `Reply`

```wolang
offer struct Reply
```

What came back. `Reply` rather than `Answer`, because a server's
answer is already called that (D144).

#### field `status`

```wolang
var status: int = 0
```

#### field `body`

```wolang
var body: text = ""
```

#### field `headers`

```wolang
var headers: [24]Pair
```

#### field `header_count`

```wolang
var header_count: int = 0
```

### function `ReplyHeader`

```wolang
offer fun ReplyHeader(answer: &Reply, name: text, fallback: text) -> text uses unsafe
```

The header called `name`, or `fallback`. Case does not matter.

### function `Fetch`

```wolang
offer fun Fetch(url: text) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

`GET url`.

### function `PostJson`

```wolang
offer fun PostJson(url: text, body: text) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

`POST url` with a JSON body.

### function `PutJson`

```wolang
offer fun PutJson(url: text, body: text) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

`PUT`, `PATCH` and `DELETE`, for an API that uses them.

### function `PatchJson`

```wolang
offer fun PatchJson(url: text, body: text) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

### function `Delete`

```wolang
offer fun Delete(url: text) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

### function `PostForm`

```wolang
offer fun PostForm(url: text, body: text) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

A form submission: `a=1&b=two`, escaped, as a browser sends it.

### function `Perform`

```wolang
offer fun Perform(call: Call) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

What a built call does. `Perform`, because `Request` is what a
server receives and `Run` is what a server does.

Redirects are followed up to the call's limit -- a `303` becomes a
`GET` with no body, which is what the status means and what every
other client does -- and `https` goes over the system's TLS (D151).

### function `Absolute`

```wolang
offer fun Absolute(from: text, location: text) -> text uses unsafe
```

A `Location` resolved against where the request went.

Absolute stays as it is; `/path` keeps the host; anything else is
relative to the directory the request was in.

### function `Once`

```wolang
offer fun Once(call: Call) -> Reply!(BadUrl, Unreachable, Truncated, NotHttp) uses io, net, time, unsafe
```

One request, one answer, no redirects.

`Connection: close` on the way out, because a client that holds a
connection has to decide when to let it go -- and a function that
answers once has nowhere to keep it. A client that wants keep-alive
uses `Open` and `Ask`.

### function `Written`

```wolang
offer fun Written(call: Call, target: Url, keep_alive: bool) -> text uses unsafe
```

The bytes of a request.

### function `Open`

```wolang
offer fun Open(url: text) -> Socket!(BadUrl, NeedsTls, Unreachable) uses io, net, unsafe
```

A connection held open, for a program making several calls.

Plain only: a kept TLS connection is `tls.Open` and `Ask` over it,
which a caller that wants one can write in three lines.

### function `Ask`

```wolang
offer fun Ask(
    connection: &mut Socket,
    target: Url,
    method: text,
    body: text,
    kind: text,
    keep_alive: bool) -> Reply!(Unreachable, Truncated, NotHttp) uses io, time, unsafe
```

One request on a connection the caller owns.
One request on a connection the caller owns.

### function `Plain`

```wolang
offer fun Plain(connection: &mut Socket, timeout: int) -> Reply!(Truncated, NotHttp) uses io, time, unsafe
```

Reads an answer off a plain connection.

### function `Secure`

```wolang
offer fun Secure(stream: &mut Stream, timeout: int) -> Reply!(Truncated, NotHttp) uses io, time, unsafe
```

The same, over TLS.

The same loop twice rather than one loop over an ability: a stream
and a socket are two types with the same three methods, and an
ability with a method per direction is more machinery than the
duplication costs (D151).

### function `Progress`

```wolang
offer fun Progress(
    received: &mut Builder,
    head: &mut int,
    length: &mut int,
    chunked: &mut bool) -> bool uses unsafe
```

Whether what has arrived is the whole answer.

The head first -- which says how the body is framed -- and then
either `Content-Length` bytes or chunks up to the empty one. A read
stops wherever it likes, which is why this is asked after every one.

### function `Finish`

```wolang
offer fun Finish(whole: text, head: int, length: int, chunked: bool) -> Reply!(Truncated, NotHttp) uses unsafe
```

The answer, with a chunked body put back together.

### function `ChunkedEnd`

```wolang
offer fun ChunkedEnd(whole: text, head: int) -> int uses unsafe
```

Where a chunked body ends, or -1 when it has not yet.

### function `FromHexDigits`

```wolang
offer fun FromHexDigits(line: text) -> int uses unsafe
```

The number a hexadecimal chunk header spells, or -1.

More than seven digits is -1: eight would overflow `int` and trap the
client on a hostile server's chunk header (D223).

A header may carry extensions after a `;`, which nothing uses and
everything has to skip.

### function `IsChunked`

```wolang
offer fun IsChunked(head: text) -> bool uses unsafe
```

Whether the head says the body comes in chunks.

### function `ParseReply`

```wolang
offer fun ParseReply(whole: text, head: int, length: int) -> Reply!NotHttp uses unsafe
```

What a response's bytes mean.

### function `ReplyNew`

```wolang
offer fun ReplyNew() -> Reply
```

An empty answer, to fill in.

### constant `kStartingRoom`

```wolang
offer const kStartingRoom: int = 8192
```

How big one connection's buffer starts out, and how far it may grow.

### struct `Limits`

```wolang
offer struct Limits
```

What a server will not read past, per request.

#### field `most_request`

```wolang
var most_request: int = 1048576
```

The whole request: head and body together.

#### field `most_head`

```wolang
var most_head: int = 16384
```

The head on its own, which is what stops a client from sending
headers forever.

#### field `keep_alive`

```wolang
var keep_alive: bool = true
```

Whether a connection is kept open after an answer.

#### field `head_timeout`

```wolang
var head_timeout: int = 10
```

Seconds a connection may take to deliver a request, counted from its
first byte (or from the accept, before it has sent any). What stops
a client that trickles one header byte at a time from holding a slot
for ever (D223).

#### field `idle_timeout`

```wolang
var idle_timeout: int = 60
```

Seconds a keep-alive connection may sit between requests.

### function `LimitsDefault`

```wolang
offer fun LimitsDefault() -> Limits
```

### struct `Held`

```wolang
offer struct Held
```

Where one connection's bytes are kept.

#### field `fd`

```wolang
var fd: int = -1
```

#### field `buffer`

```wolang
var buffer: []mut u8
```

#### field `filled`

```wolang
var filled: int = 0
```

#### field `closing`

```wolang
var closing: bool = false
```

Set when the client said `Connection: close`, or when the server
decided this one is done: the loop closes it after answering.

#### field `since`

```wolang
var since: s64 = 0
```

When the wait for the current request began, in monotonic seconds:
the accept, the end of the last answer, or the first byte of this
request, whichever is latest (D223).

#### field `served`

```wolang
var served: bool = false
```

Whether this connection has had a request answered. Until then it
owes the head deadline even with nothing sent.

### function `IsExpired`

```wolang
offer fun IsExpired(
    now: s64,
    since: s64,
    partial: bool,
    served: bool,
    limits: Limits) -> bool
```

Whether a connection that has waited since `since` has used up its
time at `now`. Pure, so the rule is testable without a clock or a
socket (D223).

A connection with part of a request in its buffer, or one that has not
yet had any request answered, is on the short head deadline; one that
is idle between keep-alive requests gets the long one.

### class `Connections`

```wolang
offer class Connections
```

A pool of connection buffers, one slot per connection held at once.

#### field `slots`

```wolang
var slots: []mut Held
```

#### field `limits`

```wolang
var limits: Limits
```

#### method `deinit`

```wolang
deinit
```

#### method `Find`

```wolang
fun Find(fd: int) -> int
```

The slot `fd` is in, or -1.

#### method `Open`

```wolang
fun Open(fd: int) -> int uses unsafe
```

Takes a slot for `fd`, or -1 when the pool is full.

A full pool is the server's back pressure: the caller closes the
connection rather than accepting work it has nowhere to put.

#### method `Release`

```wolang
fun Release(slot: int)
```

Gives the slot back. The buffer stays for the next connection: a
server that allocates per connection allocates per request under
load.

#### method `Stamp`

```wolang
fun Stamp(slot: int, now: s64)
```

Starts the slot's wait for a request at `now`.

#### method `MarkServed`

```wolang
fun MarkServed(slot: int)
```

Notes that a request on this slot was answered.

#### method `TimedOut`

```wolang
fun TimedOut(slot: int, now: s64) -> bool
```

Whether the slot holds a connection that has waited too long.

#### method `Fd`

```wolang
fun Fd(slot: int) -> int
```

#### method `IsClosing`

```wolang
fun IsClosing(slot: int) -> bool
```

#### method `SetClosing`

```wolang
fun SetClosing(slot: int, closing: bool)
```

#### method `Filled`

```wolang
fun Filled(slot: int) -> int
```

#### method `Reserve`

```wolang
fun Reserve(slot: int, wanted: int) -> bool uses unsafe
```

Makes room for `wanted` bytes in a slot's buffer, up to the limit.

False when the request wants more than the server allows, which the
caller answers with `413` rather than by growing.

#### method `Room`

```wolang
fun Room(slot: int) -> int
```

Where to read into, and how much room is there.

#### method `Fill`

```wolang
fun Fill(slot: int) -> int uses unsafe
```

Reads once from the connection into its buffer. How many bytes
arrived, zero when nothing was waiting, or negative when the
connection is finished.

#### method `Bytes`

```wolang
fun Bytes(slot: int) -> []u8
```

The bytes a slot holds.

#### method `Consume`

```wolang
fun Consume(slot: int, count: int)
```

Takes `count` bytes off the front, keeping what came after.

What makes pipelining work: two requests in one read are two turns,
and the second one's bytes are still here for the next.

### function `ConnectionsNew`

```wolang
offer fun ConnectionsNew(most: int, limits: Limits) -> Connections uses unsafe
```

### function `RequestLength`

```wolang
offer fun RequestLength(bytes: []u8, filled: int, limits: Limits) -> int uses unsafe
```

How long a request is, or what is missing.

`-1` says "not all here yet", `-2` says "this is not a request",
`-3` says "bigger than the server allows", and `-4` says "it frames
its body in a way this server does not implement" (D223).

### function `HeadEnd`

```wolang
offer fun HeadEnd(bytes: []u8, filled: int) -> int
```

Where the blank line that ends the head is, or -1.

### function `HeaderIn`

```wolang
offer fun HeaderIn(bytes: []u8, head: int, name: text) -> text uses unsafe
```

One header's value out of raw bytes, before anything is parsed.

Used for the two headers the loop itself needs -- the length and
whether to keep the connection -- which have to be read before there
is a `Request` to ask (D122).

### function `ValueAt`

```wolang
offer fun ValueAt(bytes: []u8, head: int, start: int) -> text uses unsafe
```

The value that starts at `start`, up to the end of its line.

### function `HeaderStart`

```wolang
offer fun HeaderStart(bytes: []u8, head: int, name: text, skip: int) -> int uses unsafe
```

Where the value of the `skip`th header called `name` (counting from
zero) starts, or -1 when there are not that many: a repeated
`Content-Length` has to be seen to be compared (D223). An index rather
than an optional text, so that an empty value is not "absent".

### function `QueryInt`

```wolang
offer fun QueryInt(request: &Request, name: text, fallback: int) -> int uses unsafe
```

The query parameter called `name` as a number, or `fallback`.

### function `QueryBool`

```wolang
offer fun QueryBool(request: &Request, name: text, fallback: bool) -> bool uses unsafe
```

The query parameter as a flag: `true`, `1`, `yes` and `on` are true.

### function `ParamInt`

```wolang
offer fun ParamInt(request: &Request, name: text, fallback: int) -> int uses unsafe
```

The path parameter called `name` as a number.

### function `QueryText`

```wolang
offer fun QueryText(request: &Request, name: text, fallback: text) -> text uses unsafe
```

The query parameter with `%20` and `+` read back.

### function `ParseSigned`

```wolang
offer fun ParseSigned(value: text, fallback: int) -> int uses unsafe
```

A number, with a sign, or `fallback` when it is not one.

`ParseInt` in this package stops at the first byte that is not a
digit, which is what a `Content-Length` deserves and not what a query
parameter does: `page=x` should mean "no number here" rather than
zero.

### function `ContentType`

```wolang
offer fun ContentType(request: &Request) -> text uses unsafe
```

What the request says its body is.

### function `IsJson`

```wolang
offer fun IsJson(request: &Request) -> bool uses unsafe
```

Whether the body says it is JSON.

### function `JsonBody`

```wolang
offer fun JsonBody(request: &Request) -> Value!json.Malformed uses unsafe
```

The body as JSON, or the error the parser gave.

A handler that wants to answer `400` rather than `500` for a bad
document matches on this: the body came from outside, so "this is not
JSON" is an answer and not a fault.

### function `Field`

```wolang
offer fun Field(request: &Request, name: text, fallback: text) -> text uses unsafe
```

The field called `name` from a form-encoded body.

`a=1&b=two`, which is what a browser form sends: the same shape as a
query string, so the same walk with the escapes read back.

### function `Bearer`

```wolang
offer fun Bearer(request: &Request) -> text uses unsafe
```

The token from `Authorization: Bearer <token>`, or "".

### function `Cookie`

```wolang
offer fun Cookie(request: &Request, name: text, fallback: text) -> text uses unsafe
```

The cookie called `name`, or `fallback`.

One header with `a=1; b=2` in it, which is how a browser sends them.

### function `Accepts`

```wolang
offer fun Accepts(request: &Request, kind: text) -> bool uses unsafe
```

Whether the client said it would take `kind`.

A plain substring search rather than a full parse of the header's
weights: a handler asking "will this client take JSON" wants a yes or
a no, and `*/*` is a yes.

### function `TypeOf`

```wolang
offer fun TypeOf(path: text) -> text uses unsafe
```

What a browser should make of a file, by its extension.

### constant `kMostBytes`

```wolang
offer const kMostBytes: int = 8388608
```

How much of one file this will read. A server that serves a video
wants ranges, which is a different protocol.

### function `ServeFile`

```wolang
offer fun ServeFile(path: text) -> Response uses io, unsafe
```

The file at `path`, as a response.

`404` when it is not there, which is the same answer as "you may not
have it": a server that says "no such file" for one path and
"forbidden" for another has told the asker which files exist.

### function `IsServable`

```wolang
offer fun IsServable(relative: text) -> bool uses unsafe
```

Whether `name` is something a server should hand out at all.

Not a dotfile -- `.env` and `.git` are the two every leak is made of
-- and nothing with a `..` left in it after normalizing.

### function `ServeUnder`

```wolang
offer fun ServeUnder(root: text, request: &Request, prefix: text) -> Response uses io, unsafe
```

The file under `root` that `request` asked for.

The path is taken from what the router matched for `*`, or from the
request path with `prefix` removed. Everything about it is checked:
the escapes are read back, the dots are worked out, dotfiles are
refused, and the answer has to still be inside `root`.

### function `ServeOr`

```wolang
offer fun ServeOr(root: text, request: &Request, prefix: text, fallback: text) -> Response uses io, unsafe
```

The one file a single-page application wants for every path it does
not have a file for.

### struct `Pair`

```wolang
offer struct Pair
```

One name and one value: a header, a query parameter, a path
parameter. The same shape for all three, because the work -- find the
one called this -- is the same (D112).

#### field `name`

```wolang
var name: text = ""
```

#### field `value`

```wolang
var value: text = ""
```

### constant `kMostHeaders`

```wolang
offer const kMostHeaders: int = 24
```

### constant `kMostParams`

```wolang
offer const kMostParams: int = 8
```

### struct `Request`

```wolang
offer struct Request
```

A request, as views into the buffer it was read from.

Everything here is a view: the bytes stay in the connection's buffer
until the turn is over, which is what makes reading a request cost no
allocation at all.

#### field `method`

```wolang
var method: text = ""
```

#### field `path`

```wolang
var path: text = ""
```

#### field `query`

```wolang
var query: text = ""
```

#### field `body`

```wolang
var body: text = ""
```

#### field `headers`

```wolang
var headers: [24]Pair
```

#### field `header_count`

```wolang
var header_count: int = 0
```

#### field `params`

```wolang
var params: [8]Pair
```

#### field `param_count`

```wolang
var param_count: int = 0
```

#### field `length`

```wolang
var length: int = 0
```

#### field `http_11`

```wolang
var http_11: bool = true
```

### function `RequestNew`

```wolang
offer fun RequestNew() -> Request
```

An empty request, to fill in.

The arrays are not written here: an element nobody filled is never
read, because the counts say how many there are.

### constant `kMostAnswerHeaders`

```wolang
offer const kMostAnswerHeaders: int = 8
```

What a handler answers with.

Its headers are a fixed run for the same reason a request's are: a
response is built, handed on and written, and a list that owns its
storage would make every one of those a question about who frees it
(D112).

### struct `Response`

```wolang
offer struct Response
```

#### field `status`

```wolang
var status: int
```

#### field `body`

```wolang
var body: text
```

#### field `kind`

```wolang
var kind: text
```

#### field `headers`

```wolang
var headers: [8]Pair
```

#### field `header_count`

```wolang
var header_count: int
```

### function `Header`

```wolang
offer fun Header(request: &Request, name: text, fallback: text) -> text uses unsafe
```

The value of the header called `name`, or `fallback`.

Compared without regard to case, because a client may send
`Content-Type` or `content-type` and both mean the same header.

### function `Query`

```wolang
offer fun Query(request: &Request, name: text, fallback: text) -> text uses unsafe
```

The query parameter called `name`, or `fallback`.

`?a=1&b=two` is read on demand rather than up front: most handlers
want one of them, and splitting the rest costs a pass over bytes
nobody reads.

### function `Param`

```wolang
offer fun Param(request: &Request, name: text, fallback: text) -> text uses unsafe
```

The path parameter called `name`, or `fallback`: what the router
matched for `:name`.

### function `Ok`

```wolang
offer fun Ok(body: text) -> Response
```

`200 OK`, as plain text.

### function `OkJson`

```wolang
offer fun OkJson(body: text) -> Response
```

`200 OK`, as a JSON document. The body is written as it is: a handler
that has a `Json` value writes it with `json.Write` first.

### function `Created`

```wolang
offer fun Created(location: text, body: text) -> Response
```

`201 Created`, with where the thing was created.

### function `NoContent`

```wolang
offer fun NoContent() -> Response
```

### function `NotFound`

```wolang
offer fun NotFound() -> Response
```

### function `BadRequest`

```wolang
offer fun BadRequest(why: text) -> Response
```

### function `Status`

```wolang
offer fun Status(status: int, body: text) -> Response
```

### function `WithHeader`

```wolang
offer fun WithHeader(answer: Response, name: text, value: text) -> Response
```

The same response with one more header on it. Answers the response, so
calls chain.

### function `WithType`

```wolang
offer fun WithType(answer: Response, kind: text) -> Response
```

The same response with a different content type.

### function `ParseRequest`

```wolang
offer fun ParseRequest(everything: []u8, received: int) -> Request uses unsafe
```

Splits `GET /path HTTP/1.1` into its first two words.

Returns a request whose `length` is zero when the line is not one: a
parser that cannot say "this is not a request" is not a parser.

Only the first `received` bytes are looked at: the buffer is reused
from one client to the next, and what lies past `received` belongs to
whoever used it before (D223).

### function `ParseInt`

```wolang
offer fun ParseInt(value: text) -> int uses unsafe
```

The number `value` spells, or zero. Stops at the first byte that is
not a digit, which is what a `Content-Length` with anything after it
deserves. More than nine digits is `-1`. Nine digits is this parser's own cap,
not the width of `int` (64 bits, D238); it keeps `total * 10` far from
overflow (D223).

### function `ParseLength`

```wolang
offer fun ParseLength(value: text) -> int uses unsafe
```

The number a `Content-Length` spells, or `-1` when it spells anything
else: no digits, or garbage such as `5, 10`. Spaces and tabs around
the digits are allowed. A run of more than nine digits is a length, but
no server's: it answers 2147483647, the largest `s32` and a cap this
parser keeps although `int` is 64 bits (D238), which every limit refuses
with 413 rather than the 400 that garbage gets (D223).

### function `Serve`

```wolang
offer fun Serve(connection: s32, handle: fun(Request) -> Response) -> int uses io, net, time, unsafe
```

Serves a connection until the client stops asking.

One request per turn, each inside an arena of its own: everything a
request allocates -- its response, the strings it interpolated -- comes
from there and goes back at the end of the turn. Keep-alive is what
makes that worth doing, because the connection outlives the arena.

### function `ServeWith`

```wolang
offer fun ServeWith(
    connection: s32,
    handle: fun(Request) -> Response,
    limits: Limits) -> int uses io, net, time, unsafe
```

The same, with limits of the caller's choosing.

One connection, served until the client stops asking or breaks the
rules: every request is read whole before the handler sees it, and
what arrived after it stays for the next turn (D122). A client that
says nothing for longer than `limits.head_timeout`, or
`limits.idle_timeout` between requests, is closed (D223).

### function `Turn`

```wolang
offer fun Turn(
    pool: &mut Connections,
    slot: int,
    handle: fun(Request) -> Response) -> int uses io, net, time, unsafe
```

One turn on one connection: read what is there, answer every whole
request in it, and say whether the connection is still worth holding.

`1` when a request was answered, `0` when more is wanted, and `-1`
when the connection is finished. A server that polls calls this once
per readable connection; a blocking one calls it in a loop.

### function `WriteResponse`

```wolang
offer fun WriteResponse(connection: s32, answer: Response) -> bool uses io, net, time, unsafe
```

Writes a response: the status line, the headers it carries, and the
body.

One write, because two writes of a small response wait for each other
on a socket -- the second is held back until the first is
acknowledged -- which costs more than building the string does.

### function `WriteAnswer`

```wolang
offer fun WriteAnswer(
    connection: s32,
    answer: Response,
    with_body: bool,
    keep_alive: bool) -> bool uses io, net, time, unsafe
```

The same, told whether to write the body and whether the connection
stays open.

A `HEAD` is a `GET` whose body is not written -- the length still says
what it would have been, which is the whole point of asking -- and a
client that said `Connection: close` is told the server agrees before
the socket goes away (D122).

### function `Date`

```wolang
offer fun Date() -> text uses time, unsafe
```

The date header, in the format HTTP wants.

### function `Run`

```wolang
offer fun Run(port: int, handle: fun(Request) -> Response) -> s32 uses io, net, time, unsafe
```

Listens on `port` and serves one connection after another, each inside
an arena of its own.

### function `RunPolled`

```wolang
offer fun RunPolled(port: int, most: int, handle: fun(Request) -> Response) -> s32 uses io, net, time, unsafe
```

The same server, holding many connections at once.

One loop, one thread, and a poller that says which connections have
something to say (D95). A blocking `Serve` holds one client no matter
how fast the rest of the server is; this holds as many as the system
will give descriptors for, and the cost per idle connection is one
entry in a kernel table.

`most` is how many ready descriptors one wait may report. Everything
else waits for the next turn, which is what keeps one noisy client
from starving the rest.

### function `RunServer`

```wolang
offer fun RunServer(
    port: int,
    most: int,
    handle: fun(Request) -> Response,
    limits: Limits) -> s32 uses io, net, time, unsafe
```

The polled server, with limits of the caller's choosing.

One loop, one thread, and a poller that says which connections have
something to say (D95). Each connection has a buffer of its own, so a
request that arrives in pieces is put together rather than parsed in
halves (D122).

### function `RunServerOn`

```wolang
offer fun RunServerOn(
    listener: s32,
    most: int,
    handle: fun(Request) -> Response,
    limits: Limits) -> s32 uses io, net, time, unsafe
```

The same loop, on a listener the caller opened.

What a multi-threaded server needs: each thread opens its own
listener on the same port and runs this (D135).

### struct `Before`

```wolang
offer struct Before
```

What runs before a handler. A response means "answer this and stop".

#### field `run`

```wolang
var run: fun(&mut Request) -> Response?
```

### struct `After`

```wolang
offer struct After
```

What runs after a handler, and may change what it said.

#### field `run`

```wolang
var run: fun(&Request, Response) -> Response
```

### class `Layers`

```wolang
offer class Layers
```

The layers an application runs around every request.

#### field `opening`

```wolang
var opening: List<Before>
```

#### field `closing`

```wolang
var closing: List<After>
```

#### method `Use`

```wolang
fun Use(run: keep fun(&mut Request) -> Response?) uses unsafe
```

Adds a layer that runs before the handler.

#### method `Then`

```wolang
fun Then(run: keep fun(&Request, Response) -> Response) uses unsafe
```

Adds a layer that runs after it.

#### method `Len`

```wolang
fun Len() -> int
```

### function `LayersNew`

```wolang
offer fun LayersNew() -> Layers uses unsafe
```

### function `Through`

```wolang
offer fun Through(
    layers: &Layers,
    request: Request,
    handle: fun(Request) -> Response) -> Response uses io, time, unsafe
```

Runs `handle` with `layers` around it.

The `before` layers run in the order they were added, and the first
one that answers wins -- the handler does not run at all. The `after`
layers run in reverse, so the first layer added is the outermost one,
which is the order every other server does it in and the one that
makes a logger added first see the final answer.

### function `Timing`

```wolang
offer fun Timing(_request: &mut Request) -> Response? uses time, unsafe
```

A layer pair that writes one line per request.

`Timing` goes in `before` and `Logged` in `after`; `Logger(layers)`
installs both, which is what a program actually wants.

### function `Logged`

```wolang
offer fun Logged(request: &Request, answer: Response) -> Response uses io, time, unsafe
```

### function `Logger`

```wolang
offer fun Logger(layers: &mut Layers) uses io, time, unsafe
```

Writes one line per request, with how long it took.

### function `AllowOrigin`

```wolang
offer fun AllowOrigin(origin: text)
```

What the CORS layer allows. "*" is the default and means anyone.

### function `AllowMethods`

```wolang
offer fun AllowMethods(methods: text)
```

### function `AllowHeaders`

```wolang
offer fun AllowHeaders(headers: text)
```

### function `AllowCredentials`

```wolang
offer fun AllowCredentials(allow: bool)
```

Whether a browser may send cookies. With credentials, "*" is not a
legal origin -- the browser rejects the pair -- so a program that
turns this on has to name an origin.

### function `Cors`

```wolang
offer fun Cors(_request: &Request, answer: Response) -> Response uses unsafe
```

Answers a preflight request itself, and marks every other answer.

### function `Preflight`

```wolang
offer fun Preflight(request: &mut Request) -> Response? uses unsafe
```

The other half: `OPTIONS` is answered without reaching a handler.

### function `SecurityHeaders`

```wolang
offer fun SecurityHeaders(_request: &Request, answer: Response) -> Response uses unsafe
```

The headers a browser should be told about every answer.

### function `RequireToken`

```wolang
offer fun RequireToken(token: text)
```

Requires `Authorization: Bearer <token>` on every request.

One token for the whole server: a program with users looks them up
itself, and this is for the case where a service talks to a service.

### function `Authorize`

```wolang
offer fun Authorize(request: &mut Request) -> Response? uses unsafe
```

### function `RateLimit`

```wolang
offer fun RateLimit(count: int)
```

At most `count` requests a second, across all clients.

### function `Limit`

```wolang
offer fun Limit(_request: &mut Request) -> Response? uses time, unsafe
```

### function `BodyLimit`

```wolang
offer fun BodyLimit(most: int)
```

### function `CheckBody`

```wolang
offer fun CheckBody(request: &mut Request) -> Response? uses unsafe
```

### class `Poller`

```wolang
offer class Poller
```

Watches descriptors and says which are readable.

#### field `handle`

```wolang
var handle: s32 = -1
```

#### field `ready`

```wolang
var ready: []s32
```

#### method `deinit`

```wolang
deinit
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Watch`

```wolang
fun Watch(fd: s32) -> bool uses unsafe
```

#### method `Forget`

```wolang
fun Forget(fd: int) -> bool uses unsafe
```

#### method `Wait`

```wolang
fun Wait(timeout: int) -> int uses unsafe
```

Waits up to `timeout` milliseconds and gives back how many
descriptors are ready. `At` names them.

#### method `At`

```wolang
fun At(index: int) -> int
```

### function `PollerNew`

```wolang
offer fun PollerNew(most: int) -> Poller uses io, unsafe
```

A poller that can report up to `most` ready descriptors at a time.

### function `SetNonBlocking`

```wolang
offer fun SetNonBlocking(fd: s32) -> bool uses net, unsafe
```

Stops a descriptor from blocking: a read with nothing to read returns
instead of waiting, which is what lets one loop serve many.

### struct `Call`

```wolang
offer struct Call
```

What to ask for.

Built up and handed to `Call`: `RequestNew("GET", url)` and then the
`With...` functions, each answering the request so that calls chain.

#### field `method`

```wolang
var method: text = "GET"
```

#### field `url`

```wolang
var url: text = ""
```

#### field `body`

```wolang
var body: text = ""
```

#### field `kind`

```wolang
var kind: text = ""
```

#### field `headers`

```wolang
var headers: [16]Pair
```

#### field `header_count`

```wolang
var header_count: int = 0
```

#### field `timeout`

```wolang
var timeout: int = 30000
```

How long to wait for the whole answer, in milliseconds. Zero waits
as long as the system does.

#### field `redirects`

```wolang
var redirects: int = 5
```

How many redirects to follow. Zero follows none.

#### field `verify`

```wolang
var verify: bool = true
```

Whether to check the certificate. Off is for a development server
with one it signed itself, and says so at the call.

### function `CallNew`

```wolang
offer fun CallNew(method: text, url: text) -> Call
```

### function `WithHeaderOn`

```wolang
offer fun WithHeaderOn(call: Call, name: text, value: text) -> Call
```

The same call with one more header.

### function `WithBody`

```wolang
offer fun WithBody(call: Call, body: text, kind: text) -> Call
```

The same call with a body and what it is.

### function `WithBearer`

```wolang
offer fun WithBearer(call: Call, token: text) -> Call uses unsafe
```

`Authorization: Bearer <token>`.

### function `WithBasic`

```wolang
offer fun WithBasic(call: Call, user: text, password: text) -> Call uses unsafe
```

`Authorization: Basic <user:password>`, base64 as the scheme wants.

### function `WithQuery`

```wolang
offer fun WithQuery(call: Call, name: text, value: text) -> Call uses unsafe
```

The same call with `name=value` added to the query, escaped.

### function `WithTimeout`

```wolang
offer fun WithTimeout(call: Call, millis: int) -> Call
```

The same call, waiting at most `millis` for the whole answer.

### function `WithRedirects`

```wolang
offer fun WithRedirects(call: Call, count: int) -> Call
```

The same call, following at most `count` redirects.

### function `WithoutVerification`

```wolang
offer fun WithoutVerification(call: Call) -> Call
```

The same call, without checking the certificate.

### function `JsonOf`

```wolang
offer fun JsonOf(value: Value) -> Response uses unsafe
```

`200 OK` with a JSON value, written here.

### function `Html`

```wolang
offer fun Html(body: text) -> Response
```

`200 OK` with an HTML body.

### function `Text`

```wolang
offer fun Text(body: text) -> Response
```

`200 OK` with a plain-text body and the character set said out loud.

### function `Redirect`

```wolang
offer fun Redirect(location: text) -> Response
```

A redirect to `location`.

`303 See Other` by default, which is the one that turns a POST into a
GET -- the thing a form submission wants and the reason `302` is a
trap.

### function `RedirectWith`

```wolang
offer fun RedirectWith(status: int, location: text) -> Response
```

A redirect with a status of the caller's choosing: `301` for moved for
good, `307` to keep the method.

### function `Problem`

```wolang
offer fun Problem(status: int, title: text, detail: text) -> Response uses unsafe
```

An error as a JSON document, the way an API should answer one.

The shape is RFC 7807's: a title, a status and a detail. A client that
has to tell a person what went wrong needs more than a status line,
and a body that is sometimes text and sometimes JSON is the thing that
makes writing a client miserable.

### function `Escaped`

```wolang
offer fun Escaped(value: text) -> text uses unsafe
```

A string with what JSON cannot carry escaped.

Here rather than in `std.json` because this is a *fragment* -- the
value of one member of a document being built by hand -- and the JSON
writer works on whole documents.

### function `Attachment`

```wolang
offer fun Attachment(name: text, body: text) -> Response uses unsafe
```

A body to be saved rather than shown.

### function `WithCookie`

```wolang
offer fun WithCookie(answer: Response, name: text, value: text, seconds: int) -> Response uses unsafe
```

The same response, with a cookie on it.

`Path=/` and `HttpOnly` by default: a cookie a script can read is a
cookie an injected script can read, and a program that wants one says
so with `WithHeader` itself.

### function `WithoutCookie`

```wolang
offer fun WithoutCookie(answer: Response, name: text) -> Response uses unsafe
```

The same response, with the cookie called `name` taken away.

### function `WithCache`

```wolang
offer fun WithCache(answer: Response, seconds: int) -> Response uses unsafe
```

The same response, cacheable for `seconds`.

### function `WithoutCache`

```wolang
offer fun WithoutCache(answer: Response) -> Response uses unsafe
```

The same response, which nothing may cache.

### function `WithTag`

```wolang
offer fun WithTag(answer: Response, tag: text) -> Response uses unsafe
```

The same response, with an entity tag so a client may ask again
cheaply.

### function `NotModified`

```wolang
offer fun NotModified(request: &Request, tag: text) -> Response? uses unsafe
```

`304 Not Modified` when the client already has `tag`.

A handler calls this first and answers with it when it is not null,
which is the whole of conditional GET.

### function `Unauthorized`

```wolang
offer fun Unauthorized(realm: text) -> Response uses unsafe
```

`401` with the scheme a client should use.

### function `Forbidden`

```wolang
offer fun Forbidden() -> Response
```

### function `Conflict`

```wolang
offer fun Conflict(why: text) -> Response
```

### function `Unprocessable`

```wolang
offer fun Unprocessable(why: text) -> Response
```

### function `TooMany`

```wolang
offer fun TooMany(seconds: int) -> Response uses unsafe
```

`429`, with how long the client should wait.

### function `Failed`

```wolang
offer fun Failed() -> Response
```

`500`, with nothing about what went wrong.

Deliberately: what went wrong goes in the log, where the people who
run the server can see it, and not to whoever asked.

### function `Unavailable`

```wolang
offer fun Unavailable(seconds: int) -> Response uses unsafe
```

### struct `Route`

```wolang
offer struct Route
```

One route: which method, which pattern, and what answers it.

#### field `method`

```wolang
var method: text
```

#### field `pattern`

```wolang
var pattern: text
```

#### field `handle`

```wolang
var handle: fun(Request) -> Response
```

### class `Router`

```wolang
offer class Router
```

A list of routes, tried in order.

#### field `routes`

```wolang
var routes: List<Route>
```

#### field `fallback`

```wolang
var fallback: fun(Request) -> Response = NotFoundHandler
```

#### field `has_fallback`

```wolang
var has_fallback: bool = false
```

#### method `Add`

```wolang
fun Add(method: text, pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

Adds a route. Answers nothing: a router is built once, at the top of
a program, so the calls read as a list rather than as a chain.

#### method `Get`

```wolang
fun Get(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Post`

```wolang
fun Post(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Put`

```wolang
fun Put(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Delete`

```wolang
fun Delete(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Patch`

```wolang
fun Patch(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Head`

```wolang
fun Head(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

A route for `HEAD`. Rarely needed: a `GET` route answers `HEAD` by
itself, with the body left off (D122).

#### method `Options`

```wolang
fun Options(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

#### method `Any`

```wolang
fun Any(pattern: text, handle: keep fun(Request) -> Response) uses unsafe
```

One handler for every method on a pattern.

#### method `Otherwise`

```wolang
fun Otherwise(handle: keep fun(Request) -> Response) uses unsafe
```

What answers when nothing else matches. Without one, `404`.

#### method `Mount`

```wolang
fun Mount(prefix: text, inner: &Router) uses unsafe
```

Another router's routes, with `prefix` in front of each.

Copied rather than nested: a route is a pattern and a function, so
mounting is prefixing -- and the match stays one walk over segments
instead of a walk per level (D126).

#### method `Allowed`

```wolang
fun Allowed(path: text) -> text uses unsafe
```

The methods a path has routes for, as `GET, POST`.

What `405` and `OPTIONS` both have to say, and the one question a
list of routes cannot answer without walking it twice.

#### method `Len`

```wolang
fun Len() -> int
```

How many routes there are.

### function `NotFoundHandler`

```wolang
offer fun NotFoundHandler(_request: Request) -> Response
```

What a router answers with when it has no route and no fallback.

### function `RouterNew`

```wolang
offer fun RouterNew() -> Router uses unsafe
```

### function `Together`

```wolang
offer fun Together(prefix: text, pattern: text) -> text uses unsafe
```

Two path pieces with one slash between them.

### function `Dispatch`

```wolang
offer fun Dispatch(router: &Router, request: Request) -> Response uses io, unsafe
```

Finds the route for `request`, fills in its path parameters, and
answers with what the handler said.

`404` when no pattern matches the path, and `405` when one does but
not for this method -- which is the difference between "no such thing"
and "not like that".

### function `Matches`

```wolang
offer fun Matches(pattern: text, path: text, into: &mut Request) -> bool uses unsafe
```

Whether `pattern` matches `path`, filling `params` with what `:name`
stood for.

Segment by segment: `:name` takes whatever is in that segment, `*`
takes the rest, and anything else has to be equal.

### function `ServeRoutes`

```wolang
offer fun ServeRoutes(port: int, most: int, router: Router) -> s32 uses io, net, time, unsafe
```

Listens on `port` and answers with `router`, polling for readiness.

What a program serving an API calls: the router says what answers
what, and this is the loop around it (D112).

### function `Listen`

```wolang
offer fun Listen(port: int, backlog: int) -> s32 uses net, unsafe
```

Opens a listening socket on `port`, or a negative number on failure.

### function `ListenShared`

```wolang
offer fun ListenShared(port: int, backlog: int) -> s32 uses net, unsafe
```

The same, with the port shared between threads.

`SO_REUSEPORT` gives each listener its own accept queue on the same
port and lets the system spread connections across them: one listener
per thread is what makes a multi-threaded server scale, and sharing
one listener instead makes every thread wake for every connection
(D135). On a system without it this is one listener, which still
works with one thread.

### function `Accept`

```wolang
offer fun Accept(listener: s32) -> s32 uses net, unsafe
```

Waits for the next connection.

### function `Close`

```wolang
offer fun Close(fd: s32) -> s32 uses net, unsafe
```

### function `Read`

```wolang
offer unsafe fun Read(fd: s32, buffer: *mut u8, capacity: int) -> int uses net
```

Reads into `buffer` and returns how many bytes arrived, or a negative
number. The buffer is the caller's, which is what makes this allocate
nothing per request.

### function `WriteAll`

```wolang
offer fun WriteAll(fd: s32, message: text) -> bool uses net, unsafe
```

Writes all of `message`, which may take more than one call: a socket
is allowed to accept part of what it was given.
