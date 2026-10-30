# Installing Wolang

## The one-line way

**macOS and Linux**

```sh
curl -fsSL https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.sh | sh
```

**Windows**

Download `wolang-<version>-x86_64.msi` from the
[releases](https://github.com/MowoGames/Wolang/releases) and run it: it
installs under Program Files, puts `wolang` on the PATH, and appears in
Add/Remove Programs like anything else. Silently, for a fleet:

```powershell
msiexec /i wolang-1.1.0-x86_64.msi /qn
```

Or, without the installer:

```powershell
irm https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.ps1 | iex
```

Either one downloads the build for your machine, checks it against the
published checksum, unpacks it under `~/.wolang` (or
`%LOCALAPPDATA%\Wolang`), and puts it on your PATH. Nothing is written
outside that directory and nothing asks for a password.

Then:

```sh
wolang new hello && cd hello && wolang run .
```

## Installing 1.1.0

```sh
curl -fsSL https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.sh | sh -s -- --version 1.1.0
wolang --version
```

That prints `wolang 1.1.0`. The scripts check the download against the
release's `SHA256SUMS` and stop if the checksum is wrong or missing. To
check an archive yourself, see "By hand" below. To find the newest release
without naming it, leave `--version` out.

## Reading it first

Piping a script into a shell is a thing to be uneasy about. Both scripts
are short and have no network calls beyond GitHub:

```sh
curl -fsSL https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.sh -o install.sh
less install.sh
sh install.sh
```

## Options

| | |
|---|---|
| `--version 1.1.0` | A particular release rather than the newest (`v1.1.0` works too) |
| `--no-verify` | Skip the checksum check. Without this, a release with no checksum for your archive is refused | |
| `--prefix /opt/wolang` | Somewhere else |
| `--from-source` | Clone and build instead of downloading |
| `--no-modify-path` | Leave the shell profile alone |
| `--uninstall` | Take it away |

```sh
sh install.sh --version 1.1.0 --prefix /opt/wolang
```

PowerShell takes the same as named parameters:

```powershell
.\install.ps1 -Version 1.1.0 -Prefix C:\tools\wolang -NoModifyPath
```

## Building the installer yourself

```powershell
dotnet tool install --global wix
wix extension add -g WixToolset.UI.wixext
cmake --preset windows -DCMAKE_BUILD_TYPE=Release
cmake --build --preset windows -j
pwsh tools/package/make_msi.ps1 -Build build/windows
```

That writes `build\wolang-<version>-x86_64.msi`.

## By hand

Every release has an archive per platform and a `SHA256SUMS` beside
them:

```sh
v=1.1.0
curl -fLO "https://github.com/MowoGames/Wolang/releases/download/v$v/wolang-macos-arm64.tar.gz"
curl -fLO "https://github.com/MowoGames/Wolang/releases/download/v$v/SHA256SUMS"
shasum -a 256 -c SHA256SUMS --ignore-missing    # sha256sum -c on Linux
tar -xzf wolang-macos-arm64.tar.gz
export PATH="$PWD/wolang-macos-arm64/bin:$PATH"
```

What is in the archive:

```
bin/wolang               the driver
lib/std, lib/http, ...   the libraries, as sources
lib/libwolang_*.a        the runtime and the embedding library
include/wolang/          the headers, for embedding
lib/cmake/Wolang/        find_package(Wolang CONFIG)
lib/pkgconfig/           pkg-config --cflags wolang-embed
```

## From source

```sh
git clone https://github.com/MowoGames/Wolang && cd Wolang
cmake --preset release && cmake --build --preset release -j
cmake --install build/release --prefix ~/.local
```

The full guide, including what each system needs and why it is clang, is
[building.md](building.md).

## The editor

```sh
code --install-extension wolang-1.1.0.vsix     # from the release
```

Or build it yourself — no npm needed:

```sh
python3 tools/package/make_vsix.py
```

Zed, Neovim, Sublime Text, Emacs, Helix and the JetBrains family are
supported too; see [editors/README.md](../editors/README.md).

## Platforms

| | Built | Tested in CI |
|---|---|---|
| macOS arm64 | yes | yes |
| macOS x86-64 | yes | — |
| Linux x86-64 | yes | yes |
| Linux arm64 | yes | yes |
| Windows x86-64 | yes | yes |
| Windows arm64 | through emulation | — |
| FreeBSD | from source | — |
| WebAssembly | `wolang build --target wasm32-unknown-unknown` emits an object | — |

## Keeping it up to date

Run the installer again; it replaces `current` and leaves your PATH
alone.

## Removing it

```sh
sh install.sh --uninstall        # or: .\install.ps1 -Uninstall
```

The line it added to your shell profile stays; take it out by hand. The
installer only ever writes under its prefix (`~/.wolang`), so removing that
directory is the whole uninstall. A release made with the `.msi` is removed
in Add/Remove Programs.
