# Changelog

What changed in each release, newest first. The decision behind a change
is cited as `(D<n>)` and written up in [docs/decisions.md](docs/decisions.md);
how a release is made is in [docs/releasing.md](docs/releasing.md).

## 1.3.0

1.3.0 finishes what an engine needed from the language in 1.2.0, brings
back the five libraries 1.2.0 cut, and starts writing the compiler in
Wolang. Very little in it breaks a 1.2.0 program.

### Added

- `@Save` and `std.save` (D266). A field marked `@Save` is written by name and
  read back, with shared references and cycles kept, tolerant of a field added
  or removed since the save. (The C API calls for it came in 1.4.0, D292.)
- Tasks run on the caller's clock (D267). `Tick(dt)` advances every ready
  `task fun`, `Wait(seconds)` finishes after that much ticked time, `Event` is a
  flag a task looks at, and `wolang_vm_tick` and `wolang_vm_tasks_pending` do
  the same for a VM. There is no `routine` keyword: `task fun` and `wait` were
  already the spelling for code that suspends.
- Replication attributes (D268): `@Replicated` on a field and `@Server`,
  `@Client`, `@Multicast` on a method, checked (`E0175`, `E0176`, `E0178`,
  `E0179`) and reported through the class reflection of the C API. Wolang sends
  nothing itself.
- Host controls (C API 1.4 and 1.5). `wolang_vm_set_budget` gives a call a tick
  budget so a runaway script stops with an error (D265). `wolang_vm_expect_abi`
  and `wolang_vm_expect_struct` make a binding state the layouts it was built
  for, and a mismatch fails the load (D271). A mod is never granted `unsafe`
  (D270). A trap carries its source place and the script's call stack to the
  reporter (D269). `wolang_vm_set_remap` carries the objects a host holds
  through a hot reload instead of refusing it.
- Five libraries. `font` reads TrueType and lays text out (D276). `ui` is an
  immediate-mode toolkit with a software renderer (D277). `audio` mixes and
  reads and writes WAV, and reads the headers of Ogg files (D279). `gpu` is a
  backend-neutral device with a null and an OpenGL 3.3 backend (D280). `app` is
  a fixed-timestep shell around them that runs without a window (D281).
- `std.arena`, a bump allocator held across calls (D285), and faster `std`
  copies, file reads and map slots (D284).
- `tools/selfhost`: a lexer and a parser for Wolang written in Wolang, checked
  token for token and tree for tree against the C++ ones on the repository's own
  files (D283, D288); a first tool, package fetch and lock, in Wolang (D286);
  `wolang tokens --stable` for the comparison.
- `wl bench`, and [docs/benchmarks.md](docs/benchmarks.md) with calls per
  millisecond and objects per call for each tier (D273).
- On Linux, `wolang build` can link with a bundled linker and a musl sysroot
  when the machine has no C compiler, and the same source makes the same object
  bytes (D272).
- CI rows for macOS and Windows on ARM64, and `AotTest` runs under the `asan`
  preset on Windows.

### Changed

- The binding generator registers each class with a liveness token and writes
  the layouts a binding relies on (`--no-token` leaves the token out) (D271).
- A reload that changes a class the host holds an object of now migrates the
  object when the host registered a remap, and is refused with a note when it
  did not.
- The site documentation publishes every package and 18 guides, and CI checks
  that each page converts before the publish job runs.

### Fixed

- A class with an owned `Text` field no longer corrupts the heap when its last
  handle goes, and gives the bytes back when the field is replaced (D289).
- The text an interpolation, a `+` or a `+=` builds is given back, with a
  `wolang_texts_live` counter to test it (D275).
- An annotated `T!` that holds an error with data releases its box, and an
  annotated local of an ability type keeps the annotation (D275).
- An unknown error name in a `match` arm on a `T!` is `E0166`, not the
  misleading `E0201`. A function held in a local and handed to a `@Thread`
  function is `E0163` unless the caller states `uses unsafe` (D275). A `const`
  array handed to a `[]mut T` parameter is `E0105`.
- An or-pattern trapped in the reference interpreter and not in the compiled
  tiers; comparing two enum values sent the whole function to the interpreter
  (D287). `wolang fetch` never worked on Windows (quoting for `cmd.exe`).
- `AotTest` failed to link under `asan` on Windows.

### Breaking

Little, and none of it for a program that does not use these:

- A function held in a local and passed to a `@Thread` function is `E0163`.
- A `const` array passed to a `[]mut T` parameter is `E0105`.
- A mod is never granted `unsafe`, even if the host lists it (D270).
- A reload that would change a held object's class is refused unless the host
  registered a remap.
- The C API is at 1.5. A host built against an earlier minor keeps working.

### Not done

- Ogg Vorbis audio is not decoded; the headers are read (D279).
- Nothing has run on Linux, macOS or Windows on ARM64. The bundled-linker link
  and the OpenGL backend have not run at all, and the execution budget is
  compiled for x64 only.
- `Class()` with no arguments ignores field initializers and leaves class-typed
  fields null.
- Assigning to an owned `Text` variable does not give back the old value.

## 1.2.0

The breaking language decisions land together, the gaps found since 1.1 are
closed, the embedding API grows what an engine needs, and `math` arrives.

### Added

- Operators are abilities (D239). `Self` in an ability is the type that
  implements it, and an ability marked `@Operator` lends its one method to
  an operator: `a + b` on a `Vec3` is `a.Plus(b)`, `a += b`, `-a`, `==`,
  `!=` and `< <= > >=` likewise. `std.ops` has `Add`, `Sub`, `Mul`, `Div`,
  `Neg`, `Eq`, `Ord`, `Hash`, `Max` and `Min`; numbers meet every bound
  and keep their instructions. New diagnostics `E0156`, `E0157`, `E0160`.
- Errors carry data and sets are stated (D235). `error Bad(line: int,
  why: text)` holds values, `T!(A, B)` names the errors a function can
  return, and `T!` still means any. An offered function must state its set
  (`E0150`); a return or `?` outside the set is `E0151`; a payload error
  where a `T!` is not wanted is `E0152`; a `match` on a `T!` must cover its
  errors or end with `_` (`E0201`). `json`, `yaml` and `toml` report
  `Malformed(line, column)`.
- `unsafe fun` (D236): the caller pays for `unsafe`, the body need not
  state it. A bodiless `uses unsafe` function is `E0153`; an unsafe override
  of a safe method is `E0154`.
- `@Hot` checks what a body allocates, across modules (D237, `E0155`). An
  unknown name in `uses` is `E0158`.
- `keep` on a function-typed parameter (D240). A closure that outlives its
  call owns a counted environment: it retains what it captures, copies an
  owned `text` and moves a value with a `deinit` (`E0161`, `E0162`; `keep`
  anywhere else is `E0046`).
- `[]mut T` is a writable slice and `[]T` is read-only (D251). Writing
  through a `[]T` is `E0165`. `Bytes` is no longer `unsafe`.
- `after` in a `machine` through a generated `Tick(dt)` (D260). `Self` is
  bound in `enter` and `exit`.
- Module-level `weak var` of a `C?` holds its object weakly (D253). A script
  class cannot derive from an `extern class` (`E0164`, D256).
- `a?.Method()` gives an optional of the method's result.
- Qualified names work in patterns, as intrinsic callees and as assignment
  targets (D246, `E0105`, `E0144`).
- `E0147` follows a thread body into other modules and into methods (D252).
- `math`: `Vec2`, `Vec3`, `Vec4`, `Mat3`, `Mat4`, `Quat` with operators
  through abilities (D263).
- `std.sort.SortBy`, `std.map.IntMapNew` and `TextMapNew`,
  `Builder.AppendLine`; `ServeWith` closes a connection that stays silent
  past `head_timeout`.
- Embedding, C ABI 1.1 to 1.3: functions named by module and
  `wolang_vm_function_module` (D259); a lone `.wo` can `need` (D259);
  `Print` goes through the host writer; a VM keeps its own bindings, classes
  and writers (D257); `wolang_vm_object_new`, `object_set` and
  `wolang_vm_class_*` (D258); a host class may register a token callback
  (`wolang_vm_register_class_ex`, D254).
- The binding generator binds static functions, `std::string` results and
  properties, numeric structs, `TObjectPtr` and object properties, builds
  `Quat` as `x, y, z, w`, and checks `MowoWolangAlive` before it touches an
  object (D262).
- `wolang ir --typed` prints the IR in a form `ir::Reader` reads back
  (D264). `tools/selfhost/lexer` is a throwaway lexer for Wolang in Wolang;
  the compiler will be self-hosted in stages after 1.2.0, the C++ one kept
  as the bootstrap (D250, `docs/plans/self-hosting.md`).

### Changed

- `int` is `s64` on every target and in every tier; `float` stays `f32`.
  A length, an index and a hash are `int`, so `.len` no longer stops at
  2^31 (D238). C's `int` is still `s32` in `extern` signatures: write
  `as s32` where an `int` reaches one. An integer literal with no type
  now settles on `s64`, so `E0109` fires from 2^63.
- An error's tag is the hash of its qualified name, so `json.Malformed` and
  `yaml.Malformed` are different errors.
- `std.hash.TextHash` keeps 63 bits of the hash now instead of 31, so a
  text key lands in a different slot than before; `std.map.IntHash` keeps
  its 31-bit value for every key that fit 32 bits and no longer traps for
  a larger one.
- The `alloc` effect is gone (D237). `E0136` and `E0159` are retired.
- A failed `wolang_vm_reload` keeps the old program, and a reload that
  would change a class a host holds an object of is refused with a note
  (D255).
- The embedding package no longer names LLVM's libraries; the driver and
  the tests link `wolang_aot` (D261).
- The differential fuzzer writes `int` and `s32` as different types, draws
  values at 2^31, 2^32 and the 64-bit edges, and generates stated error
  sets, payload errors and kept closures.

### Fixed

- `var a: s8 = -128` no longer traps, and `var a: s8 = 128` is `E0109`.
- `"{t}"` of an owned `Text` in a loop is no longer `E0122`.
- An alias from another package is its target; `extend` of a foreign class
  sees its fields; a used base class no longer gives an unused-import
  warning; a bodiless `extern class` method works across packages.
- Dead-code elimination and hoisting no longer drop or move a cast that can
  trap; the data of a freed slice is typed as a pointer; `ParallelFor`
  narrows its thread count.
- `wolang_vm_destroy` releases the globals a VM holds.

### Breaking

- `int` is 64 bits. Source that relied on 32-bit wrap-around, and C
  signatures that take an `int`, need `as s32`.
- AOT objects built with 1.1 do not link with 1.2.
- Text hash values changed, so anything that stored them must rehash.
- Error tags changed; anything that stored a tag must be rebuilt.
- `[]T` is read-only. Code that writes through a slice parameter writes
  `[]mut T`.
- A bodiless `uses unsafe` function is `unsafe fun` now (`E0153`).
- `alloc` is not an effect; `capabilities = "alloc"` is `E0053`.
- A `T!` returned from an offered function states its errors (`E0150`),
  and a `match` on a `T!` that does not cover them is `E0201`.

## 1.1.0

The first public release after an audit of the compiler, the runtime, the
sandbox and the libraries that read other people's bytes, and the fixes
for what it found.

### Upgrading from 0.1

Some programs that compiled before are now refused, because what they did
was unsound or silently wrong:

- Code calling `Free`, `ArrayNew`, `ViewFromRaw`, `std.text.Bytes` or a
  thread body that touches module-level vars has to state `uses unsafe`.
- A dependency written as a bare string in `wolang.toml` has to become
  `{ path = ... }` or `{ git = ..., tag = ... }`.
- `after` on a machine transition is refused (`E0159`); it never waited.
- An integer literal too wide for `int` needs its type (`const e: s64 = ...`).
- `x += 1` used to store `1`. Code that relied on that was already broken.
- Embedding hosts must be rebuilt: `WolangValue` is 32 bytes, calls return
  a `WolangStatus`, and `WOLANG_ABI_VERSION` says which shape a header has
  (D241).

### Safety

- A bodiless `uses unsafe` primitive (`Free`, `ArrayNew`, `ViewFromRaw`)
  no longer absorbs `unsafe`, which closed a way for a sandboxed mod to
  reach raw memory (D221). Naming a function as a value costs its effects
  (D222), and so does calling or naming a method. Calling `std.text.Bytes`
  is `unsafe`, because its view can write a literal.
- A reference cannot leave through a field, an index, a call or a local
  array turned into a slice (`E0121`), and an object made in an `arena`
  cannot leave its block (`E0142`, D228).
- A `deinit` that keeps `self` traps in every tier (`E0145` for the plain
  case); a move out of a `match`, out of a closure's captures or of a
  by-value parameter is tracked, so a double move is `E0122` (D229).
- Enums and structs that carry a `deinit` value drop it, and move it, with
  the whole.
- Module-level vars and captures are refused in thread bodies, and a
  sandboxed package cannot declare an `extern` nested in a type.
- The HTTP server parses only the bytes that arrived (it used to read the
  previous client's), refuses a `Content-Length` that would overflow and
  any `Transfer-Encoding`, and closes slow and idle connections (D223).
  A hostile chunk size no longer traps the client.
- JSON, YAML, TOML and regex bound their depth, and
  `InflateLimited`/`GunzipLimited` bound their output (D227).
  `Normalize` refuses a path with too many parts.

### Language

- `need std.json` now also gives `json.Parse`, `json.Value` and so on:
  every `need` makes its last path segment a qualifier. When two modules
  offer one bare name and a file uses it, that is `E0143`; an unknown
  qualified name is `E0144` (D246).
- A literal too wide for `int` is `E0109` instead of wrapping; runaway
  generic instantiation is `E0140`; recursive enums and generics held by
  value are `E0117`; an expression chain too deep for the later passes is
  `E0041` (D225).
- Float literal arithmetic is done at the float type the context asks for.
- A transition on a machine cannot carry `after`; task cancellation is
  struck from the spec.
- Compound assignment (`+=`, ...) is a read-modify-write, and a module-level
  var of function type is called through its storage.

### Libraries

- New package `lib/image`: PNG, QOI and BMP codecs, written in Wolang
  (D245).
- `std.net` has UDP sockets for game networking.
- A dependency in `wolang.toml` can say which versions it accepts, and a
  bare-string dependency is refused (D242).
- `gfx` gains cursor modes (hidden, locked for mouse-look, shapes),
  fullscreen, window size and position, DPI-change and file-drop events,
  up to four gamepads, and native message boxes and file dialogs (D247).
- `lib/db`, `lib/tls` and `lib/image` are tested in CI on every system.

### Embedding

- The C ABI has the shape it will keep: `WOLANG_ABI_VERSION` and
  `wolang_abi_version()`, a 32-byte `WolangValue`, `WolangStatus` results,
  a sized `WolangVmOptions` for `wolang_vm_create_with`, and
  `<lib>_abi_version` exported by `wolang build --library` (D241).

### Tiers

- The interpreter, the JIT and LLVM AOT agree on integer edges: labelled
  `break`/`continue` on range-`for` and `while`, 64-bit overflow and
  `MIN / -1` trap instead of folding, `u64` interpolates above 2^63, float
  `%` is `fmod`, and sign-changing 64-bit casts trap (D224, D226).
- Trap text is identical in every tier, and floats print as the shortest
  round-trip text with every NaN as `nan`.
- The differential fuzzer covers every integer width, casts, shifts and
  bitwise operators, labelled loops, `match`, enums, optionals, `T!`,
  text, classes and live-object counts; it found and closed seven more
  disagreements in narrow signed arithmetic, payload reads, text copies
  and unsigned folding.
- The interpreter stops before the native stack runs out, so deep
  compile-time recursion is a diagnostic.

### Tooling and CI

- `wolang --version` reports the version the project was built as: it is
  written once, in `CMakeLists.txt` (D248).
- Releases come from one tag push: Linux x86-64 and arm64 and Windows
  x86-64 archives (macOS builds from source for now), a Windows
  installer, the editor extension and a `SHA256SUMS`, built with the
  LLVM back end so `wolang build` works. `install.sh` and `install.ps1`
  verify the checksum and refuse to install without one (D248).
- AOT is built and fuzzed on every system, there is a UBSan job, and
  `fuzz --aot` fails without a back end instead of passing.
- Diagnostic messages escape control bytes; `wolang fuzz` writes its
  output by length.
- The specification, diagnostics catalogue and roadmap were corrected
  against the code.

## Older versions

Before 1.1.0 the project was `0.1.x` and had no published releases; there
is nothing to upgrade from. The editor extension kept its own history in
[editors/vscode/CHANGELOG.md](editors/vscode/CHANGELOG.md).
