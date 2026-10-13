# Wave 3 — incremental compilation, packages, formatter

**Status:** complete. 257 unit tests, all passing in Debug, sanitizers and
Release.

## Scope

Make the compiler incremental, give it a notion of packages and modules, and
add the formatter. The exit criterion — editing a function body recomputes
that function only — is what the whole wave was built around.

## What was built

### Query engine — `include/wolang/query`

A demand-driven cache with red-green invalidation. Facts are either **inputs**
(`SourceText`, `ModuleFiles`, `Workspace`) or **derived** values computed from
other queries.

Three properties do the work:

- **Dependencies are discovered, not declared.** Every `Get()` a computation
  makes is recorded in a frame, so a computation and its dependency list can
  never drift apart.
- **Nothing is invalidated eagerly.** Changing an input bumps a global
  revision. The next `Get()` verifies dependencies and recomputes only what
  actually moved.
- **Back-dating.** A query that is recomputed and produces a value *equal* to
  its predecessor keeps its old `changed_at` stamp, so everything downstream
  stays green. `MakeComparableResult` opts a query into this; results without
  a comparison always propagate.

A computation that asks for a query already running sets `cycle_detected`
instead of recursing, so a cyclic definition becomes a diagnostic rather than
a stack overflow.

`QueryEngine::Seed` installs a value computed by an earlier process together
with the dependencies it came from — the primitive the on-disk cache is built
on. A seeded value is not special: it is verified and invalidated like any
other.

Instrumentation (`ComputationCount`, `total_verifications`, `total_hits`) is
part of the public interface, because "recomputed only what was needed" is a
property of the engine that tests have to be able to assert.

### Item trees — `include/wolang/compiler/item_tree.h`

The pivot of the design. An `ItemTree` holds each declaration's kind, flags,
name, parent and a hash of its signature — and deliberately holds **no source
positions and no syntax handles**, both of which shift when a body is edited.
That is what lets it compare equal after a body-only edit, which is what stops
invalidation from spreading.

The signature hash covers attributes, modifiers, generics, parameter and
return types, base types, effect lists, and a machine's transitions. It
deliberately excludes bodies, initializers, and the *value* of a default
argument (though not its presence).

`ResolveItem` walks the tree in the same order to recover a declaration from
an entry index, so positions are resolved on demand against the current
revision.

### Database — `include/wolang/compiler/database.h`

Owns the source manager, the interner and the engine, and registers the
queries: `Tokens`, `ParsedFile`, `ItemTree`, `ModuleScope`, `Imports`,
`FileDiagnostics`.

Two invariants worth recording:

- A file's text changes only through `SetFileText`, which sets the
  `SourceText` input. A computation may read the source manager directly as
  long as it asked for the matching `SourceText` query first.
- Diagnostics belong to the result that produced them, not to a shared engine.
  A query can be recomputed at any time, so a cached result has to carry its
  own messages or they would be duplicated or lost.

Cross-file diagnostics resolve their spans against the syntax tree of the
revision being reported, never against anything stored in a cached value.
`ResolvedImport` therefore stores an ordinal rather than a span, and
`ModuleScope` stores item tree indices.

Files carry a `FileRole`: a manifest lives in the source manager so its
diagnostics can point at a line, but it is not Wolang source and is never
lexed as such.

### Manifests, modules and workspaces

- `manifest.cc` parses a small TOML subset with eleven diagnostics
  (`E0050`–`E0054`) and keeps whatever it understood, so one bad line does not
  discard a project.
- `module.cc` builds a module scope from the item trees of a module's files,
  in a deterministic order so two computations of the same module compare
  equal.
- `workspace.cc` discovers packages and modules on disk. Discovery touches the
  filesystem, so it is explicitly *not* a query; loading a workspace bumps the
  `Workspace` input. Directory entries are sorted so module membership never
  depends on filesystem iteration order.

Import resolution reports unresolved modules (`E0055`), unknown names
(`E0056`), duplicate declarations (`E0057`), non-public names (`E0058`),
package cycles (`E0059`) and unreadable packages (`E0060`). Per-code severity
comes from the owning package's `[lints]` table.

### Formatter — `include/wolang/format`

Renders a tree back to source in one canonical style, with the margin as the
only option. It works from the tree rather than from the text, so it is
idempotent by construction — with two things it has to handle by hand:

- **Parentheses are not in the tree.** They are re-derived from precedence, so
  `(1 + 2) * 3` keeps its parentheses and `1 + (2 * 3)` loses them. The
  precedence table moved to `token_kind.h` and is now shared with the parser,
  which is what guarantees the two agree.
- **Comments are not in the tree.** They are re-attached from the token
  stream's comment table by position, as leading lines or as a trailing
  comment on the line just written. A construct containing a comment is always
  written in its broken-out form, because the one-line form has nowhere to put
  it.

Floats are printed at the shortest precision that reads back identically, with
`.0` appended when the digits alone would lex as an integer — otherwise `15.0`
would come back as `15` and change type.

### On-disk cache — `include/wolang/compiler/cache.h`

Stores item trees and nothing else: they are the expensive-to-reach,
cheap-to-store summaries, and anything that needs tokens or a tree is about to
read the file anyway.

Correctness-neutral by construction. Every entry carries the hash and size of
the text it came from, the file carries a format version, and the reader is
byte-exact rather than a struct dump. A stale, corrupt, truncated or foreign
cache is discarded. Seeding never overwrites a value the database already
computed.

### Driver

`wolang check` now accepts package directories as well as files, and uses the
cache automatically. New: `wolang fmt` (with `--check` and `--write`),
`wolang modules`, and `--no-cache`.

## Decisions taken in this wave

| Decision | Rationale |
|---|---|
| Modules are directories, not files | adding a file never requires editing a declaration, and it matches how a game splits into plugins |
| The manifest stays declarative TOML | external tools must be able to read a package's name and version without a Wolang interpreter; a build script can come later alongside it, as `build.rs` does beside `Cargo.toml` |
| Item trees hold no positions | a cached summary that moves when a body is edited defeats its own purpose |
| Diagnostics live in query results | a shared engine would duplicate or lose messages across recomputations |
| Discovery is not a query | the engine can only track facts it is told about; the filesystem is not one of them |
| The cache stores item trees only | tokens and trees are large and are needed only when the file is about to be read anyway |
| A file outside a package resolves no imports | checking one file is a legitimate thing to do and must not invent errors |
| The formatter attaches a closing delimiter to the last item | matches the Chromium style the project's own C++ uses, so contributors see one convention |

## Bugs this wave found

- **Struct patterns lost a binding name.** `Point { y: inner }` stored the
  field name over the binding name, so `inner` vanished. `Pattern` now has
  separate `name` and `field`, and the formatter prints the short form only
  when they agree.
- `PrintAst` required a `SourceManager` it did not always need; it now takes a
  pointer, which is what lets the item tree render an expression canonically.
- `WOLANG_DCHECK` compiled its condition away entirely in release builds,
  making debug-only helpers unused. It now evaluates `sizeof` on the
  expression instead.

## Things worth remembering

- `Symbol` is an index into one compilation's interner. Two `Database`
  instances number the same name differently, which is why the cache stores
  name *text* and the cache test compares trees by their spelled-out form.
- `SourceManager::ReplaceText` invalidates every `string_view` previously
  handed out for that file. Cached values store offsets, not views, which is
  what makes this safe.
- `FindFile` is a linear scan. Fine for hundreds of files, wrong for tens of
  thousands; it wants a map when a project gets large.

## Deferred to later waves

- Parallel query evaluation. The engine is single-threaded; the design (pure,
  deterministic queries) allows it, but nothing needs it yet.
- Caching token streams and syntax trees, if profiling ever shows item trees
  are not enough.
- A dependency registry and `build.wo`.
- `wolang fmt` does not yet break long binary expressions or chains; it only
  breaks argument and parameter lists.

## Next

Wave 4: types, generics, traits, nullability and units. See `STATUS.md`.
