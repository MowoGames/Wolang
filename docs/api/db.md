# db

## db

### error `Unreadable`

```wolang
offer error Unreadable
```

What a store that cannot be opened or read answers with.

### class `Store`

```wolang
offer class Store
```

A key-value store backed by one file.

#### field `handle`

```wolang
var handle: s32 = -1
```

#### field `path`

```wolang
var path: text = ""
```

#### field `index`

```wolang
var index: List<Entry>
```

#### field `live`

```wolang
var live: int = 0
```

#### field `dead`

```wolang
var dead: int = 0
```

#### field `at_end`

```wolang
var at_end: s64 = 0
```

#### method `deinit`

```wolang
deinit
```

#### method `IsOpen`

```wolang
fun IsOpen() -> bool
```

#### method `Len`

```wolang
fun Len() -> int
```

How many keys there are.

#### method `Wasted`

```wolang
fun Wasted() -> int
```

How many records the log holds that nothing points at any more.
`Compact` is worth calling when this is most of the file.

#### method `Has`

```wolang
fun Has(key: text) -> bool uses unsafe
```

Whether `key` is there.

#### method `Get`

```wolang
fun Get(key: text) -> text? uses unsafe
```

What `key` holds, or nothing.

#### method `Put`

```wolang
fun Put(key: text, value: text) -> bool uses unsafe
```

Puts `value` under `key`, replacing what was there.

The record is on the disk when this answers true -- `fsync`, not
"the system will get to it", because a store that loses the last
write is a store nobody can use for what a store is for.

#### method `Delete`

```wolang
fun Delete(key: text) -> bool uses unsafe
```

Takes `key` away. True when it was there.

#### method `Keys`

```wolang
fun Keys() -> List<text> uses unsafe
```

Every key, in order.

#### method `Range`

```wolang
fun Range(from: text, to: text) -> List<text> uses unsafe
```

Every key from `from` up to but not including `to`, in order.

What a sorted index is for: `Range("user:", "user;")` is every
user, and a log-structured store usually cannot answer it.

#### method `Compact`

```wolang
fun Compact() -> bool uses unsafe
```

Writes the live records to a new file and swaps it in.

What reclaims the space a replaced or deleted record still takes.
Safe to interrupt: the new file is written whole and renamed, so
either the old store or the new one is there, never half of one.

#### method `Find`

```wolang
fun Find(key: text) -> int uses unsafe
```

#### method `LowerBound`

```wolang
fun LowerBound(key: text) -> int uses unsafe
```

#### method `Remember`

```wolang
fun Remember(key: text, offset: s64, length: int, gone: bool) uses unsafe
```

#### method `ReadValue`

```wolang
fun ReadValue(entry: Entry) -> text? uses unsafe
```

### function `Open`

```wolang
offer fun Open(path: text) -> Store!Unreadable uses io, unsafe
```

Opens the store at `path`, making it when it is not there.

### function `Close`

```wolang
offer fun Close(store: &mut Store) uses io, unsafe
```

Closes the store. Reading or writing after this answers nothing.
