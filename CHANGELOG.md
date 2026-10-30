# Changelog

What changed in each release, newest first. The decision behind a change
is cited as `(D<n>)` and written up in [docs/decisions.md](docs/decisions.md);
how a release is made is in [docs/releasing.md](docs/releasing.md).

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
- Releases come from one tag push: five platform archives, a Windows
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
