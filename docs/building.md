# Building Wolang

The compiler, the runtime and the libraries need **clang** and **CMake
3.24+**. Nothing else. LLVM is optional and only for `wolang build`.

> **Why clang.** The compiler uses clang's overflow builtins for the
> checked arithmetic every Wolang operator compiles to, `__int128` for
> the wide multiply a constant divisor becomes, and statement
> expressions in the trap machinery. GCC has most of those and is
> untested; Microsoft's compiler has none of them, which is why the
> Windows build uses **clang-cl** (D184).

## In one minute

```sh
git clone https://github.com/MowoGames/Wolang && cd Wolang
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug
build/debug/tools/wolang/wolang run examples/hello.wo
```

## What each system needs

### macOS

Apple's clang is fine, and it is what `xcode-select` installs.

```sh
xcode-select --install
brew install cmake ninja
# Optional, for `wolang build` and for cross-compiling:
brew install llvm
```

If CMake does not find LLVM, point it at the one Homebrew installed:

```sh
cmake --preset release -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
```

### Linux

```sh
sudo apt install clang lld cmake ninja-build        # Debian, Ubuntu
sudo dnf install clang lld cmake ninja-build        # Fedora
sudo pacman -S clang lld cmake ninja                # Arch

# Optional: the ahead-of-time backend, and a window
sudo apt install llvm-dev libx11-dev
```

If `cc`/`c++` are GCC, say which compiler to use:

```sh
CC=clang CXX=clang++ cmake --preset debug
```

### Windows

Install [LLVM](https://github.com/llvm/llvm-project/releases) (it
provides `clang-cl`), [CMake](https://cmake.org/download/) and
[Ninja](https://ninja-build.org/), then from a *Developer Command
Prompt* — the Visual Studio C++ build tools provide the system headers
and libraries that clang-cl uses:

```bat
cmake --preset windows
cmake --build --preset windows -j
ctest --preset windows
```

The `windows` preset selects `clang-cl` explicitly. Building with
Microsoft's own compiler does not work and the configure step says so.

The ahead-of-time tier (`wolang build`) needs LLVM's C API, which the
LLVM installer does not include. Fetch it once, then configure again:

```bat
python tools/package/fetch_llvm.py
cmake --preset windows
```

That puts LLVM 22.1.8's headers, `LLVM-C.lib` and `LLVM-C.dll` in
`build/llvm-sdk`; the build copies the DLL next to `wolang.exe`. LLVM
23 miscompiles a remainder in a comparison (D208), so it is 22 on
purpose.

## The presets

| Preset | What it is for |
|---|---|
| `debug` | Everything on, warnings are errors. What to develop against. |
| `release` | `-O2`, no assertions. What to measure and ship. |
| `asan` | Address and undefined-behaviour sanitizers. Where memory bugs die. |
| `ubsan` | Undefined-behaviour sanitizer alone, recovery off; cheaper than `asan`, run in CI. |
| `windows` | clang-cl, on Windows. |
| `x64` | Cross-build to x86-64 on an arm64 machine, for the other back end. |

```sh
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
```

## Options

| Option | Default | What it does |
|---|---|---|
| `WOLANG_WITH_LLVM` | `ON` | The ahead-of-time backend. Off builds without `wolang build`. |
| `WOLANG_ALL_TARGETS` | `ON` | Emit code for every target LLVM has, so `--target` works. Off is this machine only, and a much smaller driver. |
| `WOLANG_BUILD_TESTS` | `ON` | The unit tests. |
| `WOLANG_BUILD_TOOLS` | `ON` | The `wolang` driver. |
| `WOLANG_BUILD_PLATFORM` | `ON` | The window, GPU and sound library. |
| `WOLANG_WARNINGS_AS_ERRORS` | `OFF` | On in the `debug` preset. |
| `WOLANG_ENABLE_SANITIZERS` | `OFF` | On in the `asan` preset. |
| `WOLANG_REQUIRE_CLANG` | `ON` | The check above. Off to try another compiler. |

```sh
cmake --preset release -DWOLANG_ALL_TARGETS=OFF -DWOLANG_BUILD_TESTS=OFF
```

## Checking the other back end without another machine

There are two machine-code back ends -- arm64 and x86-64 -- and a laptop
runs one of them. On an Apple Silicon machine the `x64` preset builds a
real x86-64 binary, and Rosetta runs it:

```sh
cmake --preset x64 && cmake --build --preset x64 -j
./build/x64/tests/wolang_tests          # the whole suite, as x86-64
./build/x64/tools/wolang/wolang fuzz --seed 1 --count 150
```

This is worth doing after any change to a back end. The first time it was
run, 94 tests into the suite the x86-64 build crashed, and seven real
bugs were behind it (D195 to D198).

When something does crash there, the crash is in generated code and has
no source line on it. What to do:

```sh
wolang asm --raw file.wo                # the bytes of each function, hex
```

Then hand those bytes to a disassembler -- `llvm-mc --disassemble
--arch=x86-64` takes `0x..` separated by spaces -- and compare what the
two back ends emitted for the same program. The one that answers
correctly is the specification.

```sh
# The same function from both, side by side.
wolang asm --raw probe.wo | sed -n '/@Probe:/,$p'
```

## What gets built

```
build/<preset>/
  tools/wolang/wolang          the driver: run, build, test, fmt, lsp...
  src/libwolang_core.a         the compiler
  src/libwolang_runtime.a      what a built binary links against
  src/libwolang_embed.a/.so    the embedding API
  platform/libwolang_platform.*  window, GPU and sound
  tests/wolang_tests           the unit tests
```

## Running the tests

```sh
W=build/debug/tools/wolang/wolang

ctest --preset debug          # the compiler's own tests
$W test lib/std               # the standard library, in two tiers
$W test lib/http lib/db lib/gfx
$W fuzz --count 200           # the tiers against each other
$W fmt --check lib examples   # the formatter
python3 tools/package/check_docs.py   # every example in the docs
```

The fuzzer is the one that matters after a change to lowering, a back
end or the runtime: it generates programs, runs them through all three
tiers and compares.

## Installing

```sh
cmake --install build/release --prefix /usr/local
```

That puts `wolang` in `bin`, the libraries in `lib`, the headers in
`include/wolang`, and the standard library's sources where the driver
looks for them. See [install.md](install.md) for the scripts that do
this for you.

## Building the editor extension

No npm needed:

```sh
python3 tools/package/make_vsix.py
code --install-extension build/wolang-0.1.0.vsix
```

## When something goes wrong

**`Wolang is built with clang, and this is GNU`** — set `CC` and `CXX`,
as above.

**`no LLVM found`** — the build continues without `wolang build`. Install
LLVM, or pass `-DLLVM_DIR=...`.

**`ld: library 'LLVM-22' not found`** on macOS — CMake found LLVM's
headers from one installation and its libraries from another. Pass
`-DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"`.

**Tests pass but `wolang run` traps in a package of yours** — run it with
`--interpret`. A difference between the two is a compiler bug and a good
bug report (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

**A window does not open on Linux** — the platform library needs X11
headers at build time (`libx11-dev`); without them `gfx.Available()`
answers false and everything else still works.
