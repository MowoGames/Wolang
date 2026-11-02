# tls 0.1.0

## tls

### class `Stream`

```wolang
offer class Stream
```

A TLS connection over a socket.

The socket is held so that closing the stream closes both: a TLS
connection whose socket outlives it is a descriptor nobody owns.

#### field `session`

```wolang
var session: s64
```

#### field `socket`

```wolang
var socket: Socket
```

#### field `open`

```wolang
var open: bool = false
```

#### method `deinit`

```wolang
deinit
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Send`

```wolang
fun Send(message: text) -> bool uses unsafe
```

Sends everything in `message`. False when the connection broke.

#### method `Receive`

```wolang
fun Receive(limit: int) -> text uses unsafe
```

Reads up to `limit` bytes. "" when the other end closed.

#### method `Close`

```wolang
fun Close() uses unsafe
```

### function `StreamNone`

```wolang
offer fun StreamNone() -> Stream
```

A closed stream, which is what a failure answers with.

### function `Handshake`

```wolang
offer fun Handshake(host: text, port: int, verify: bool) -> Stream uses net, unsafe
```

Connects to `host` on `port` and shakes hands.

`Handshake` rather than `Open`, because a program that speaks both
HTTP and TLS imports both and this language has no import aliases
(D150).

The certificate is verified against the system's own roots, and the
name is checked against it: TLS that does not check the name is TLS
that any server can answer (D150). `verify = false` is for a
development server with a certificate it signed itself, and says so
at the call.
