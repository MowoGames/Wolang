# http

## http

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
offer fun Header(request: &Request, name: text, fallback: text) -> text
    uses unsafe
```

The value of the header called `name`, or `fallback`.

Compared without regard to case, because a client may send
`Content-Type` or `content-type` and both mean the same header.

### function `Query`

```wolang
offer fun Query(request: &Request, name: text, fallback: text) -> text
    uses unsafe
```

The query parameter called `name`, or `fallback`.

`?a=1&b=two` is read on demand rather than up front: most handlers
want one of them, and splitting the rest costs a pass over bytes
nobody reads.

### function `Param`

```wolang
offer fun Param(request: &Request, name: text, fallback: text) -> text
    uses unsafe
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
offer fun WithHeader(answer: Response, name: text, value: text)
    -> Response
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
offer fun ParseRequest(bytes: []u8, received: int) -> Request uses unsafe
```

Splits `GET /path HTTP/1.1` into its first two words.

Returns a request whose `length` is zero when the line is not one: a
parser that cannot say "this is not a request" is not a parser.

### function `ParseInt`

```wolang
offer fun ParseInt(value: text) -> int uses unsafe
```

The number `value` spells, or zero. Stops at the first byte that is
not a digit, which is what a `Content-Length` with anything after it
deserves.

### function `Serve`

```wolang
offer fun Serve(connection: s32, handle: fun(Request) -> Response) -> int uses io, unsafe
```

Serves a connection until the client stops asking.

One request per turn, each inside an arena of its own: everything a
request allocates -- its response, the strings it interpolated -- comes
from there and goes back at the end of the turn. Keep-alive is what
makes that worth doing, because the connection outlives the arena.

### function `WriteResponse`

```wolang
offer fun WriteResponse(connection: s32, answer: Response) -> bool
    uses io, unsafe
```

Writes a response: the status line, the headers it carries, and the
body.

One write, because two writes of a small response wait for each other
on a socket -- the second is held back until the first is
acknowledged -- which costs more than building the string does.

### function `Run`

```wolang
offer fun Run(port: int, handle: fun(Request) -> Response) -> s32 uses io, unsafe
```

Listens on `port` and serves one connection after another, each inside
an arena of its own.

### function `RunPolled`

```wolang
offer fun RunPolled(port: int, most: int, handle: fun(Request) -> Response) -> s32 uses io, unsafe
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
fun Watch(fd: int) -> bool uses unsafe
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
offer fun PollerNew(most: int) -> Poller uses unsafe
```

A poller that can report up to `most` ready descriptors at a time.

### function `SetNonBlocking`

```wolang
offer fun SetNonBlocking(fd: int) -> bool uses unsafe
```

Stops a descriptor from blocking: a read with nothing to read returns
instead of waiting, which is what lets one loop serve many.

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

#### method `Add`

```wolang
fun Add(method: text, pattern: text,
          handle: fun(Request) -> Response) uses unsafe
```

Adds a route. Answers nothing: a router is built once, at the top of
a program, so the calls read as a list rather than as a chain.

#### method `Get`

```wolang
fun Get(pattern: text, handle: fun(Request) -> Response) uses unsafe
```

#### method `Post`

```wolang
fun Post(pattern: text, handle: fun(Request) -> Response) uses unsafe
```

#### method `Put`

```wolang
fun Put(pattern: text, handle: fun(Request) -> Response) uses unsafe
```

#### method `Delete`

```wolang
fun Delete(pattern: text, handle: fun(Request) -> Response) uses unsafe
```

#### method `Len`

```wolang
fun Len() -> int
```

How many routes there are.

### function `RouterNew`

```wolang
offer fun RouterNew() -> Router uses unsafe
```

### function `Dispatch`

```wolang
offer fun Dispatch(router: &Router, request: Request) -> Response
    uses io, unsafe
```

Finds the route for `request`, fills in its path parameters, and
answers with what the handler said.

`404` when no pattern matches the path, and `405` when one does but
not for this method -- which is the difference between "no such thing"
and "not like that".

### function `Matches`

```wolang
offer fun Matches(pattern: text, path: text, into: &mut Request)
    -> bool uses unsafe
```

Whether `pattern` matches `path`, filling `params` with what `:name`
stood for.

Segment by segment: `:name` takes whatever is in that segment, `*`
takes the rest, and anything else has to be equal.

### function `ServeRoutes`

```wolang
offer fun ServeRoutes(port: int, most: int, router: Router) -> s32
    uses io, unsafe
```

Listens on `port` and answers with `router`, polling for readiness.

What a program serving an API calls: the router says what answers
what, and this is the loop around it (D112).

### constant `kFamilyInet`

```wolang
offer const kFamilyInet: s32 = 2
```

The address family and socket kind a TCP listener needs.

### constant `kStream`

```wolang
offer const kStream: s32 = 1
```

### function `Listen`

```wolang
offer fun Listen(port: int, backlog: int) -> s32 uses unsafe
```

Opens a listening socket on `port`, or a negative number on failure.

The address is built byte by byte because a `sockaddr_in` is a C struct
and this language has no way to say that yet: sixteen bytes, of which
the first four are the ones that matter. The layout is the BSD one that
macOS uses -- a length byte, then the family -- and a port in network
order, which is to say most significant byte first.

### function `Accept`

```wolang
offer fun Accept(listener: s32) -> s32 uses unsafe
```

Waits for the next connection. Blocking: readiness notification is what
wave 13 brings, and until then a connection is served before the next
one is taken.

### function `Close`

```wolang
offer fun Close(fd: s32) -> s32 uses unsafe
```

### function `Read`

```wolang
offer fun Read(fd: s32, buffer: *mut u8, capacity: int) -> int uses unsafe
```

Reads into `buffer` and returns how many bytes arrived, or a negative
number. The buffer is the caller's, which is what makes this allocate
nothing per request.

### function `WriteAll`

```wolang
offer fun WriteAll(fd: s32, message: text) -> bool uses unsafe
```

Writes all of `message`, which may take more than one call: a socket is
allowed to accept part of what it was given.
