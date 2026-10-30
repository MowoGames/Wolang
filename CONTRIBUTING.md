# Contributing to Wolang

Thank you for looking. This file is what you need to know to send a
change that can be merged.

## The short version

Requires **clang** and CMake 3.24+ — see
[docs/building.md](docs/building.md) for why and for what each system
needs.

```sh
git clone https://github.com/MowoGames/Wolang && cd Wolang
cmake --preset debug && cmake --build --preset debug -j
ctest --preset debug                       # the unit tests
build/debug/tools/wolang/wolang test lib/std   # and the library's own
```

Then make your change, add a test that fails without it, and run:

```sh
W=build/debug/tools/wolang/wolang
ctest --preset debug
$W test lib/std && $W test lib/http && $W test lib/db
$W fuzz --count 200          # the tiers against each other
$W fmt --check lib examples  # the formatter
```

If all four are quiet, open a pull request.

## What this project values

**The three tiers must agree.** An interpreter, a just-in-time tier and
an ahead-of-time tier run the same program. A change that makes them
disagree is a bug even when each answer looks reasonable on its own, and
`wolang fuzz` exists to find exactly that. If you touch lowering, a
back end or the runtime, run the fuzzer with a few seeds.

**A test that would have caught it.** Every bug fix comes with the test
that fails before the fix. Where it goes:

| What changed | Where the test goes |
|---|---|
| The compiler, a back end, the runtime | `tests/unit/...`, as a `WOLANG_TEST` |
| A library written in Wolang | a `@Test` in the same file |
| Something only a real program shows | an example under `examples/` |

**Comments say why.** The code says what it does; a comment is for the
reason it does it that way, and for the thing that will not be obvious in
six months. Decisions that shaped the design live in
[docs/decisions.md](docs/decisions.md) with a number, and comments point
at those numbers.

**No dependencies.** The compiler, the runtime and the libraries depend
on clang and nothing else. LLVM is optional and only for
`build`. If a change needs a third-party library, say why in the pull
request before writing it — the answer is sometimes yes, and it is never
automatic.

## Style

C++ follows the [Chromium style guide](https://chromium.googlesource.com/chromium/src/+/main/styleguide/c++/c++.md):
two spaces, eighty columns, `PascalCase` functions, `snake_case` files,
`trailing_underscore_` members, `kPascalCase` constants, no exceptions
and no RTTI. Wolang follows [docs/style.md](docs/style.md).

Run the formatter on Wolang code; there is no formatter for the C++ yet,
so match the file you are in.

## Commits

One line, in the imperative, following
[Conventional Commits](https://www.conventionalcommits.org):

```
feat(std): read and write TOML
fix(lower): wrap a value returned from a fallible function
docs: explain what a wave is
```

Scopes that exist: `lexer`, `parser`, `sema`, `ir`, `codegen`, `interp`,
`runtime`, `std`, `http`, `gfx`, `db`, `tls`, `lsp`, `editors`, `docs`,
`ci`, `build`.

The title is read by people as it is: every push to `alpha` posts the
titles of its commits to the studio's changelog in Discord and Telegram,
and a release tag posts everything since the previous one
(`.github/changelog.py`); a pull request is one line with its title and
a link. A failed `ci`, `docs` or `release` run is posted to the CI
channel (`.github/notify.sh`) with the failed step and the end of its
log, and the first green run after it says so.

## Where things are

```
include/wolang/   public headers, mirrored by src/
  syntax/         tokens, lexer, syntax tree, parser
  sema/           types, resolution, effects, dimensions, flow
  ir/             typed SSA, lowering, passes, the verifier
  codegen/        the dev tier (arm64, x64) and the LLVM back end
  interp/         the reference tier
  runtime/        allocator, threads, sockets, files, text
tools/wolang/     the driver
lib/              std, http, gfx, db, tls, game, core -- written in Wolang
platform/         window, input, GPU and sound, one backend per system
tests/            unit tests and the test harness
docs/             specification, architecture, decisions, diagnostics
```

Start with [docs/architecture.md](docs/architecture.md) for how a program
becomes a binary, and [STATUS.md](STATUS.md) for what is done and what is
not.

## Where to start

Issues labelled
[`good first issue`](https://github.com/MowoGames/Wolang/labels/good%20first%20issue)
are small and self-contained; issues labelled
[`help wanted`](https://github.com/MowoGames/Wolang/labels/help%20wanted)
are larger and nobody is on them. If you want to take one, say so on the
issue so two people do not do the same work.

Beyond the issue list, these are open and are good ways in. Each one is
in [STATUS.md](STATUS.md) with the context around it.

| Size | Area | What is missing |
|---|---|---|
| small | docs | A program from the tour or the cookbook that you wished existed. Every example is compiled by CI, so a wrong one fails. |
| small | `std` | `strftime`-style patterns for formatting and parsing dates; today only ISO 8601 is understood. |
| small | `http` | A cookie jar and `multipart/form-data` in the client. |
| small | `editors` | A grammar or an LSP setup for an editor that is not in [editors/](editors/). |
| medium | `codegen` | Strength reduction for any constant divisor, not only powers of two. |
| medium | `codegen` | Spill slots that are reused instead of one frame area per temporary. |
| medium | tools | Reading an IR dump back in; the printer is stable, the reader does not exist. |
| medium | tools | Version ranges for dependencies; today a dependency is an exact revision. |
| deep | `ir` | Alias analysis, and the vectorisation that needs it. |
| deep | `codegen` | Windows on arm64: the arm64 emitter with the Windows calling convention. |

Something larger, or a change to the language itself, starts as a
[discussion](https://github.com/MowoGames/Wolang/discussions) or a
feature issue rather than as a pull request. The language has a
specification and about two hundred numbered decisions behind it, and a
short conversation first saves a long review later.

## Reporting a bug

Open an issue with the smallest program that shows it, what you expected,
and what happened. These help a great deal:

```sh
wolang check  path   # what the compiler says about it
wolang ir     path   # the IR, which says what it understood
wolang run --interpret path   # the reference tier's answer
```

A disagreement between tiers — one answer from `run`, another from
`run --interpret` — is the most valuable kind of report and will be
treated as such.

## Security

Do not open an issue for a vulnerability. See
[SECURITY.md](SECURITY.md).

## Licence

By contributing you agree that your work is licensed under Apache-2.0,
the same as the project.
