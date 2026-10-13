# Packages and modules

## Layout

A **package** is a directory with a `wolang.toml` manifest. A **module** is a
directory of `.wo` files inside it:

```
mygame/
├── wolang.toml
└── src/
    ├── main.wo              module mygame
    ├── combat/
    │   ├── damage.wo        module mygame.combat
    │   └── weapons.wo       module mygame.combat
    └── engine/
        └── physics/
            └── body.wo      module mygame.engine.physics
```

A module's name is the package name followed by the path below the source
root. Every `.wo` file in one directory belongs to one module; a directory
with no `.wo` files declares no module.

Modules are directory-scoped rather than file-scoped on purpose: adding a file
never requires editing a module declaration, and the layout matches how a game
is already split into plugins and subsystems.

Directories whose name starts with `.` are tooling, not source, and are
skipped — including the compiler's own `.wolang-cache`.

## The manifest

```toml
[package]
name = "mygame"
version = "0.2.1"
# Where modules live, relative to the manifest. Defaults to "src".
source = "src"

[dependencies]
core = { path = "../core" }
# The short form means the same thing:
math = "vendor/math"

[lints]
# Any diagnostic code, set to off, hint, perf, warning or error.
W0105 = "off"
P0101 = "error"

[build]
# "deny" forbids `unsafe` blocks anywhere in this package.
unsafe = "allow"
```

Only `[package].name` is required. Unknown keys and tables are reported rather
than ignored, so a typo in a key name is caught instead of silently doing
nothing.

The format is a deliberately small subset of TOML: tables, string values and
inline tables. A package manifest needs nothing more, and a small parser with
precise diagnostics is worth more than a complete one with vague ones.

Only path dependencies exist today. A registry is a later wave; a build script
(`build.wo`, in the spirit of `build.rs`) becomes possible once the language is
executable in wave 7. The manifest stays declarative data either way, so that
any external tool — CI, a registry, a dependency bot — can read a package's
name and version without running a Wolang interpreter.

## Visibility

| | |
|---|---|
| no modifier | visible inside the module |
| `pub` | visible to other modules |
| `priv` | visible inside the file only |

A module's declarations are shared across its files, so two files in one
directory see each other's names without importing anything. Declaring the
same name twice in one module is an error (`E0057`), reported on both
declarations.

## Imports

```wolang
import mygame.combat                      // the whole module
import mygame.combat.{Damage, Weapon}     // selected names
```

An import names a module, not a file. Importing a name the module does not
declare is `E0056`; importing one it declares but does not export is `E0058`.

Cycles between packages are an error (`E0059`). Cycles between modules inside
one package are fine: a module's names are known from its declarations, which
do not depend on the modules it imports.

## Checking without a package

A single file can be checked on its own:

```sh
wolang check src/combat/damage.wo
```

Such a file belongs to no module, so its imports are not resolved and no
import diagnostics are produced — lexical, syntactic and later type errors are
still reported in full. This is what makes a scratch file or a bug report
reduction usable.

## The cache

```
mygame/.wolang-cache/items.wdb
```

Checking a package directory writes this file and reads it next time. It holds
the item tree of every source file — each declaration's kind, name, nesting
and signature hash — together with the hash and size of the text it came from.
A file whose text still matches skips lexing and parsing entirely.

The cache is a pure optimization and is never trusted: a changed file, a
corrupt file, a truncated file or one written by a different compiler build is
discarded. `--no-cache` skips it in both directions, which is how a suspected
cache problem is ruled out.

## Commands

```sh
wolang check <paths>      # files or package directories
wolang modules <dir>      # print the package and module graph
wolang fmt <files>        # print the formatted form
wolang fmt --check <f>    # fail if a file is not formatted
wolang fmt --write <f>    # rewrite in place
wolang ast <files>        # print the syntax tree
wolang tokens <files>     # print the token stream
```

`examples/package` is a working package to try these on.
