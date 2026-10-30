# Phase 2: pre-1.0 language decisions D235-D243 (+ spec cleanup)

Verify with `W=.claude/scripts/wl`. Before writing this plan I checked these against the built driver (debug build at e6249d0):
- F-A: `match Root(30) { Ok(v) => .., Small => .. }` compiles, then traps "unreachable" at run time.
  The error set is never inferred. Every `T!` resolves to `Fallible(T, ErrorSet({}))`
  (resolution.cc `Resolve` kFallible), and `Accepts` takes any error set.
- F-A2: an error's tag is `name.index + 1`, the interned *name* (lower.cc `FallibleTest`, D90). So
  json/yaml/toml/ini/compress `Malformed` are one tag. A payload would make this unsound.
- F-B: the tour's `uses io, unsafe` on `main` is unnecessary. Print/ParseInt/Sqrt/RandomNew all absorb.
  Only Threads (D232) and Talking to C (extern) need it. Point.Length's `uses unsafe` is also unneeded.
- F-F: `fun Keep(f: fun(int)->int) { saved = f }` stores a capturing closure passed down. Then
  `saved(1)` prints garbage (the frame is gone), and lowering reports "passes 1 arguments to @K$saved,
  which takes 0". E0136 only looks at a closure *literal* (type_check.cc `RefuseEscape`).
- F-I: `on Interact after 3 -> Open` fires at once. The delay is parsed (parser.cc `ParseTransition`)
  and nothing reads it.
- Codes E0143/E0144/E0146/E0149 and E0150-E0159 are unused. D230/D231 are absent from decisions.md.
  I take that as reserved elsewhere and leave them alone.

## A. D235 Errors carry data; a set is stated or open (E0150-E0152)
**Choice: payloads on error tags. Not `Result<T, E>`.** `Result` needs generic enums, and there are
none: no parser, layout or monomorphisation support exists. It would also give the language two error
mechanisms, plus conversions between `E` types at every `?`, which `T!` was designed to avoid.
**Decision text:** "**D235. An error can carry data, and a set is written or open.** `error
Malformed(line: int, column: int)` is an error with a payload. A function states which errors it
gives back, `-> Value!(Malformed, TooDeep)`, or writes `T!` and means *any*. Inference was promised
and never built: every `T!` accepted every error, so a `match` that missed one compiled and trapped.
A written set is checked: returning or `?`-passing an error outside it is `E0151`. A `match` over it
is exhaustive (E0201). An open `T!` needs a catch-all arm. An offered function states its set
(`E0150`), for the reason it states its effects: a caller in another module cannot see the body. A
payload is boxed in a counted object beside the tag, so a set with no payload errors keeps today's
two-field layout. An open `T!` carries no payload (`E0152`), because its layout cannot depend on a
body. Errors are told apart by declaration, not by name: a tag is a hash of `module.Name`, the same in
every tier, every build and every reload."
**Syntax:** `error A, B(x: int, why: Text)`. Types: `T!` is open, `T!A` and `T!(A, B)` are stated,
`T?!(A)`. A function type may state a set too. Construct with `return B(3, "x")`. Match with
`B(x, why) => ..`; a bare `B =>` ignores the payload; `Ok(v) =>`.
**Codes:** E0150 means an `offer fun` returns an open `T!`. E0151 means the body fails with an error
its stated set lacks; this includes a `?` from an open set. E0152 means a payload error flows into an
open `T!`. A tag-hash collision reuses the duplicate-declaration code.
**Layers.** A1 does stated and open sets plus identity. A2 does payloads.
- syntax: parser `ParseType` (`!` then an optional name or `(list)`); `ParseErrorDecl` (a payload
  field list after each name); the AST holds the TypeRef error list and the Item variant fields.
  formatter: print both.
- sema: resolution.cc `Resolve` kFallible gives an open set (a new `TypeStore::OpenErrorSet()` sentinel)
  or a real `ErrorSet(decl ids)`. Error items are keyed by declaration (Binding kErrorTag gets
  `declaration`). type.cc gets a `ErrorSubset(a, b)` helper. type_check.cc: `Accepts` (subset rule);
  the return/`?` checks raise E0151 and E0152; exhaustiveness for kFallible lists the stated members
  and requires a catch-all when the set is open; `BindPattern` binds payload types (A2). The E0150 check
  sits beside E0704 (database.cc `kUnstatedPublicEffect` site).
- ir: lower.cc `FallibleTest` and `LowerTry` change; the tag becomes `ErrorTag(decl)`, which is FNV-1a
  of the qualified name, never 0. A2: the layout of a set with payload errors is `{u32 tag, T, detail}`,
  where detail is a counted handle to a synthesized class `$err.Name`. `Coerce`/`LowerTry` copy or null
  the detail. abi/layout.cc `FieldCount`/`FieldType` read 3 fields only when `HasPayload(set)`.
  `IsFallibleCounted` covers detail. No new opcode: the work is MakeStruct/Extract/class construction
  and the counted ops, so the verifier and the back ends are unchanged.
- cache: item-tree signatures carry the set, so bump `Cache::kVersion`.
- lib migration (27 signatures): http client errors, json/yaml/toml/ini `Malformed`, regex
  `BadPattern`, compress, db `Unreadable`. All are written sets. A2 gives `Malformed(line, column)` to
  json/yaml/toml (the user-visible payoff).
- docs: spec §4.3 rewritten (drop the `Result` sentence), §3 sentence, the tour "Optionals and
  failures" (`Root -> int!(Negative, TooBig)`), a cookbook entry "Report where a parse failed",
  diagnostics.md.
- tests (TypeCheck): the F-A program is E0201 today and traps, so it is the failing test. E0150
  fire/twin: `offer fun F() -> int!` vs `-> int!(A)`. E0151 fire/twin. E0152 fire/twin. json.Malformed
  vs yaml.Malformed are distinct (LowerTest). A2: CodegenTest `PayloadErrorsAgree`, a match reading a
  payload through `?` in all tiers, plus AotTest. A lib @Test for the json line/column.
  Add `T!` with tag-only errors to `src/fuzz/generator.cc` so the fuzzer covers `?` propagation.
- risks: A2's counted detail is new drop work; a leak or a double release shows in tiers differently.
  CodegenTest plus fuzz is the proof, and the asan preset is run on the payload test. ABI: `T!` never
  crosses the C ABI (WolangKind has no kind for it). Hot reload: hash tags make a carried `T!` global
  survive a reload. Effort: A1 M, A2 M-L.

## B. D236 `unsafe fun` is the caller's promise; `uses unsafe` is the author's (E0153-E0154)
**Need:** yes. `socket.Read(fd, buffer: *mut u8, capacity)`, `AtomicAdd(counter: *mut s64)` and
`gfx.TextFromC` have bodies, absorb `unsafe`, and are unsafe for any wrong argument. Today a mod
reaches them with no `unsafe` of its own.
**Decision text:** "**D236. `unsafe fun` is a promise the caller makes; `uses unsafe` is one the
author made.** A body that states `uses unsafe` vouches for itself and absorbs it (D35, D221). Some
bodies cannot: a function whose safety depends on the pointer or length it is given is only as safe
as its caller. `unsafe fun` says so. Its body may do unsafe things without stating it, and every call
or naming of it (D222) charges `unsafe` to the caller, as `extern` does. A bodiless primitive is
written `unsafe fun`. `uses unsafe` there said one thing and meant the other (`E0153`). An `unsafe
fun` cannot override or implement a method that is not one (`E0154`): a call through the base would
skip the promise."
**Layers:**
- parser: the `unsafe` modifier before `fun` (also `offer unsafe fun`). Today the `unsafe {` block is
  parsed in expression position only. The ItemFlags gain `kUnsafe`.
- item tree and cache: the flag goes into the signature hash.
- sema: resolution `Binding::is_unsafe_fun`. In type_check.cc `ChargeBinding`, an unsafe fun always
  adds unsafe whatever `has_body` says, and its body's direct unsafe ops are allowed. E0153/E0154 go
  in database.cc effect diagnostics and in the override/ability conformance check.
- formatter, LSP hover, stdlib.md generator.
- lib migration: std.mem `BytesFromRaw, SoaNew, ArrayNew, Free, ViewFromRaw, Prefix, Suffix` become
  `unsafe fun`. The 3 pointer wrappers above too. Callers that then need `uses unsafe` follow the
  compiler's hints.
- docs: spec §5.3 (one paragraph replaces the D221 sentence), the tour "Talking to C" gets 4 lines on
  the two spellings (the `main` cleanup is in J1).
- tests (EffectRowTest/TypeCheck): an unsafe fun with a body charges its caller (fails today: absorbs).
  E0153 fire (bodiless `uses unsafe`) / twin (`unsafe fun`). E0154 fire/twin. Sandbox: `unsafe =
  "deny"` package calling `socket.Read` is E0801. Effort S-M. Sema only, no tier risk.

## C. D237 Allocation is a checked property, not a stated effect (E0155)
**Recommend: implement for `@Hot`, and remove `alloc` from what signatures state.** Inferring `alloc`
as a row effect would put `uses alloc` on every offered function that builds text (E0704); there are
0 such today and hundreds would need it. A mod's grant gains nothing from it either: every mod
allocates. `@Hot` is what the engine needs from it (`Tick`).
**Decision text:** "**D237. What allocates is worked out, and only `@Hot` asks.** `alloc` was an effect
nobody inferred, so `@Hot` accepted anything. Allocation is now a property of a body, found the way
D232 finds a thread's globals. Constructing a class, `ArrayNew`, building a `Text` (interpolation,
`Builder`), a boxed error (D235) and a boxed environment (D240) set it. A call or a naming (D222)
passes it on, across modules and through methods, because `List.Push` is in another module and is a
method. A `@Hot` function that reaches it is `E0155`, with the chain. It is not a row effect: stating
it on every function that formats a message would cost everyone, for one attribute's benefit. `uses
alloc` and the `alloc` capability are gone."
**Layers:** type_check records `result_.allocates` (sites listed above) and the method-call edges
(D232 skipped them). database.cc adds a program-wide `kAllocates` query, run only when some module has
a `@Hot`, so there is no cost otherwise. It sits next to the D232 thread pass and closes over modules
(modules may import each other, so it is one query, not one per module). E0155 is reported at the
`@Hot` item. The `@Hot` row loses `kAlloc` (database.cc ~L1037). The effect table drops `alloc`. The
manifest's `capabilities = "...alloc"` gets W-level "no longer a capability" (or E0053, owner's pick).
Spec §5, §5.2, §5.3 examples. Tests: module_unittest's `@Hot` test becomes E0155 through a class
construction, a cross-module `List.Push`, and interpolation. A twin: a `@Hot` doing arithmetic only.
Deferred: `arena { }` exemption, virtual calls (conservative: any override that allocates), calls
through a held function value (charged where named, D222). Effort M. Sema only.

## D. D238 `int` is 64 bits
**Decision text:** "**D238. `int` is `s64`.** A length, an index and a hash are `int`, and `int` was 32
bits, so a collection, a file read into memory or a hash table stopped at 2^31. Making only `len`
wider would break every `var i = 0 ... i < xs.len`, because there are no mixed-width operations (D33).
Making `int` wider breaks almost nothing written in the language. A word-sized `isize` would differ
between wasm32 and x86-64, and one answer on every target is the rule this project keeps. The
boundaries keep their widths: C's `int` is `s32` in `extern` and in bindings, and a stored format
writes an explicit width. `float` stays `f32`: that is the GPU's width, and a different question."
**Rejected:** `len: s64` with `int = s32` (every loop breaks), `usize/isize` (target-dependent).
**Incremental:** yes, in 3 steps. D1 pins boundaries with no behaviour change: every `extern` in lib
using `int` for C `int` becomes `s32`; db's on-disk ints, gfx C structs and hash masks get explicit
widths; bind already maps int to s32 (bind.cc, reflection.cc). D2 flips `TypeStore::default_int()` and
the `int` alias, then fixes the test fallout (overflow-at-2^31 tests, E0109 cases, expected output),
the spec §4 table and §4.1, the tour. D3 removes the 32-bit masks in std.map/std.hash and the
2^31 capacity checks in List/Text, and widens the fuzzer's int domain.
**Risks:** tier disagreement at the new edges (D224 folding: MIN/-1 on s64 already handled). 64-bit
`idiv` is slower on x64. Struct sizes grow. A host bound function declared `-> int` meaning C int
silently becomes s64: D1 audits this and the bind generator already emits s32. Effort: D1 S, D2 M-L,
D3 S.

## E. D239 `Self`, and operators through abilities (E0156-E0157)
**Decision text:** "**D239. An ability can name the type that implements it, and an operator is an
ability.** `Self` in an ability is the implementing type: a projection like `Item` (D212), so the
machinery that rewrites `Item` onto its owner rewrites `Self`, and every monomorphised copy is
concrete. Inside `extend T` or a type's own body `Self` is `T`. Anywhere else it is `E0156`. An
ability that takes or returns `Self` cannot be a value type (`E0157`): a pair of data and table cannot
say what `other` is. An ability marked `@Operator` lends its one method to an operator: `Plus +,
Minus -, Times *, Divide /, Negate -x, Equals == !=, Compare < <= > >=`. `std.ops` offers `Add, Sub,
Mul, Div, Neg, Eq, Ord` and `Hash`. `a + b` on a `Vec3` is the call `a.Plus(b)`, decided in the
checker, so lowering sees an ordinary call and the tiers have nothing new to agree on. The ability is
required, not just the method's name: relaxing that later breaks nothing, and tightening it would."
**Shapes:** `Plus(other: Self) -> Self`, `Equals(other: &Self) -> bool`, `Compare(other: &Self) ->
int`, `Hash() -> u64`.
**Layers:** resolution (`Self` binding in ability/extend/type scopes; `@Operator` recorded on
abilities). The D212 projection gets owner = ability and name = Self, resolved where `Item` is. The
conformance check of `extend T as A` substitutes Self:=T. type_check `CheckBinary`/unary: a non-numeric
operand looks up its `@Operator` implementation (concrete, or through a bound in a generic body) and
records the callee on the expr. lower.cc's binary lowering emits the call when the record is present
(one branch). The E0157 check sits where an ability is used as a value type. New lib module
`lib/std/src/ops/ops.wo`. `std.sort` gains `SortBy<T: Ord>` as a first user. A Map-by-ability is
deferred (D91 values stay). Docs: spec §3/§4.4, the tour "Abilities" (Vec3 `+`), cookbook.
**Tests:** TypeCheck: `Vec3 + Vec3` with `extend Vec3 as Add` works (fails today). E0156 fire (`Self`
in a free fun)/twin. E0157 fire (`const e: Eq = ..`)/twin (`Hash` as a value is fine). A generic
`Max<T: Ord>` in CodegenTest across tiers. A std.ops @Test. Effort M.

## F. D240 Escaping closures own their environment (E0158)
**Decision text:** "**D240. A closure that outlives its call owns what it captured.** A function value
stays two words. A parameter of function type is borrowed: it may point at the caller's frame, so
storing it, returning it or passing it to a `keep` parameter is `E0158`. `keep fun(...)` is a
parameter the callee may keep. Fields, globals and results of function type are kept. A capturing
closure that flows into a kept place gets its environment as a counted heap object. Its code reads the
captures from there, its value is retained when copied and released when dropped, and the last
release drops what it captured. A closure that does not escape is exactly what it was, with the same
frame, no header and no count. That is the zero-cost case, and it is most closures. Before this,
E0136 looked only at a literal, so a closure passed down and stored by the callee dangled."
**Cost:** zero for borrowed and non-escaping values. A kept function value pays one null test per
copy/drop: plain functions have no environment, and every non-null environment in a kept place is
counted. The C ABI is unchanged: a C callback still takes the code word, and a capturing closure to an
`extern` parameter stays refused.
**Layers:** F0 fixes the bug: a call through a module-level `var` of function type resolves to the
constant initializer `K$name` (lower.cc call lowering and ConstantName). F1 adds the parser's `keep`
contextual modifier in parameter types; type.h's function type gains `kept`; type_check's
`RefuseEscape` is generalised to E0158 for borrowed values; signals' `Connect` is keep. lib
migration: `MapNew`, `SetNew`, `http` router/handlers, `log` sinks, signal users get `keep`. E0136
stays for capturing literals into kept places until F2. F2: `LowerClosure` makes an escaping literal
build `$env.N` (a synthesized counted class whose fields are the captures, plus a `$drop` releasing
them) instead of the stack tuple. `Counts()`/`HoldsCounted` treat kept function types as counted (env
word nullable). E0136 is retired. D229's "captured deinit value never dropped" is fixed for heap
environments. `@Thread(captures)` copies (D147) also retain the counted env.
**Tests:** F0 LowerTest/CodegenTest calling `saved(1)` through a global (fails today). F1 E0158 fire
(the F-F program) / twin (`keep` param given a plain function). F2 CodegenTest+AotTest: a counter
closure returned, called 3 times, and dropped, run against a deinit capture to prove the release;
asan preset. Add closures to the fuzz generator. Effort F0 S, F1 M, F2 L.

## G. D241 The C ABI's frozen shape
**Decision text:** "**D241. The embedding ABI says its version and keeps its shape.** `wolang.h` gains
`WOLANG_ABI_VERSION` (major<<16 | minor) and `wolang_abi_version()`, so a host checks at load what it
was compiled against. A `WolangValue` is a kind, a reserved word and one union of the scalar or handle,
beside a text and its length: 32 bytes where it was 56. The field names stay, so host source compiles
unchanged. `kind` is a `uint32_t`, because an enum's size is the C compiler's choice. Kinds are only
ever added, and a host must accept one it does not know. A struct that will grow starts with its
size: `WolangVmOptions` for `wolang_vm_create_with`. Calls answer a `WolangStatus` instead of 'non
zero'. MowoEngine's generated glue calls through C functions and words (D218, D219), not values, so
it does not change."
**Layers:** include/wolang/embed/wolang.h, wolang.hpp; src/embed/embed.cc (every `WolangValue`
construction and read, around 30); examples/embed; `wolang build --library` exports
`<lib>_abi_version`. Tests: EmbedTest checks the version and round-trips every kind. A new C11
translation unit `tests/unit/embed/abi_c_check.c` asserts `sizeof` and `offsetof` (built by CMake).
Effort M. No tier risk (embed sits above the tiers).

## H. D242 Version requirements are written and checked, not resolved
**Decision text:** "**D242. A dependency can say which versions it accepts.** `std = { path = "..",
version = "^1.2" }`, or with `git`. The grammar is Cargo's: `1.2` means `^1.2`; `~`, `=`, `>=`, `<`,
comma for 'and', `*`. There is no resolution: the one copy found must satisfy every requirement on it,
or the build says which two disagree. A bare string, `std = "1.2"`, is a version, which is what every
other manifest means by it. It is an error until there is a registry, rather than a path as before."
**Layers:** manifest.h `ManifestDependency::version_req`. manifest.cc: the inline-table splitter honours
quotes (commas inside `">=1, <2"`), a `ParseVersionReq`, and bare-string handling. workspace.cc checks
the loaded package's `[package] version` and records it in the lock. Codes E0052 and E0053. Tests
(Manifest/Workspace): a satisfied and an unsatisfied requirement, the comma in quotes, the bare string.
Effort S.

## I. D243 `after` is refused, and tasks are not cancelled (E0159)
**Recommend:** reject `after` now, and strike structured cancellation from the spec. A clock for
machines means a generated `Tick(dt)`; cancelling a suspended frame means drop code at every suspend
point (L). Neither is on the 1.0 path.
**Decision text:** "**D243. What a machine and a task do not do yet is refused, not ignored.** `on X
after 3s` parsed and then fired at once (D71 said so; the compiler did not). It is now `E0159`. The
syntax stays reserved for a machine that is given time by a generated `Tick(dt)`. §8 promised that a
task is cancelled with the scope that spawned it, and nothing cancels a task. A spawned task runs to
its end. Stopping one early is a flag it reads, and `std.tasks` says so."
**Layers:** type_check (or systems.cc where machine tables are checked) reports E0159 at
`Transition::delay`. Check whether `wait` in `enter` is refused the same way; if it is silently
lowered, report E0159 there too and say it in the decision. Spec §8, §9 examples. Tests (TypeCheck):
E0159 on the F-I program (it silently mis-runs today) / twin without `after`. Effort S.

## J. Spec cleanup (no D-number; J1 now, J2 after the waves)
J1: §1.5 `task` (and `while`) break the keyword table's 8-column rows, so reflow them. §3 `offer offer
fun`. §4.5 rewritten: explicit type arguments exist (`Box<int>(7)`, `Get<Position>(world, e)` in this
same spec), and so does monomorphisation. §4 table: `Option<T>` (no such type: `T?` only), `Array<T>`
(it is `std.list.List<T>`), `Weak<T>` (it is `weak var`, D207); check `Set`/`Shared`. §11's last
paragraph contradicts "Running the systems", where systems run, in parallel stages, on threads (D214).
§12 `Array<float>.Generate` does not exist. The sections after §12 (Returning a value, Walking a
collection with D212 `Iterable`, Threads, Building a binary, Running the systems, Closures, Serving an
API, Editors, Debugging): the normative ones (Returning, Walking, Closures) become numbered §13-§15,
and the rest move to cookbook.md/tour.md. The tour drops `unsafe` from every `main` except Threads and
Talking to C, and from `Point.Length`. J2 sweeps after each item lands its own section, plus STATUS.md.
`W docs` checks every moved `fun main` block. Effort S-M.

## Order and dependencies
F0, I, H, G, J1, D1 depend on nothing. D2 goes after D1, and is easier before A/E/F tests get written.
A2 after A1. F2 after F1. C before A2/F2, so they charge alloc at their sites; otherwise they add it
afterwards. B, C, E, A1 and F1 are independent of each other. J2 comes last.

## Parallel waves (one coder per line; lines in a wave touch different functions)
- Wave 1: F0 (lower.cc call lowering), I (machine check), H (manifest/workspace), G (embed), J1 (docs
  only), D1 (lib externs/db/hash). Disjoint files except lib: D1 touches extern lines only.
- Wave 2: D2 alone. It flips the default and touches tests suite-wide; then D3 in the same run or
  right after.
- Wave 3: A1, B, C, E, F1. Shared files and how they stay apart: parser.cc (A: `ParseType`/error decl;
  B: item modifier; F1: parameter type; E: none), type_check.cc (A: Accepts/fallible/patterns/
  exhaustiveness; B: `ChargeBinding`; C: new `result_.allocates` sites; E: `CheckBinary` and
  conformance; F1: `RefuseEscape`), database.cc (A: by E0704; B: effect diagnostics; C: new query
  next to D232's). `Cache::kVersion` is bumped by whoever merges last; each coder notes it. Formatter
  conflicts are in different print functions.
- Wave 4: A2 and F2. Both edit lower.cc `Counts`/`HoldsCounted`/`IsFallibleCounted`, so the second to
  merge rebases that block. Run `W fuzz 200` and the asan preset for each.
- Wave 5: J2 and STATUS.md (`wolang-wrapup`).

## Steps (each one coder run; verification after the arrow)
1. F0: global fun-var call → `W build && W test CodegenTest && W fuzz 200`
2. I: E0159 + spec §8/§9 → `W test TypeCheck && W docs`
3. H: version requirements → `W test Manifest && W test Workspace`
4. G: ABI shape + C check → `W build && W test EmbedTest && W docs`
5. J1: spec/tour cleanup → `W docs`
6. D1: pin boundary widths in lib → `W lib && W test BindTest`
7. D2+D3: `int` = s64 → `W ci && W fuzz 200` (plus `W build asan && W test` once)
8. A1: written/open sets, E0150-E0151, hash tags, lib signatures → `W test TypeCheck && W test LowerTest && W lib && W fuzz 200`
9. B: `unsafe fun`, E0153-E0154, std.mem migration, tour → `W test EffectRowTest && W test TypeCheck && W lib && W docs`
10. C: allocates query, E0155, drop `alloc` → `W test ModuleScopeTest && W test Database && W lib && W docs`
11. E: `Self`, `@Operator`, std.ops, E0156-E0157 → `W test TypeCheck && W test CodegenTest && W lib std && W fuzz 200`
12. F1: `keep`, E0158, lib migration → `W test TypeCheck && W lib && W docs`
13. A2: payloads, E0152, json line/column → `W test CodegenTest && W test AotTest && W lib && W fuzz 200`
14. F2: boxed environments, retire E0136 → `W test CodegenTest && W test AotTest && W lib && W fuzz 200` + asan
15. J2: spec sweep, diagnostics.md, STATUS.md → `W ci`

## Deferred, explicitly
Named error sets (`T!Parse` naming an alias), `Result`, generic enums. `arena` exemption for `@Hot`.
`after` with a clock. Task cancellation. A Map keyed by `Eq + Hash`. Dynamic (`dyn`) abilities with
`Self`. Version resolution and pre-release ordering. Dependency ranges across a registry.
