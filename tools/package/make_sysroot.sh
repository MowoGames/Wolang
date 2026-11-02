#!/bin/sh
# Builds the sysroot `wolang build` links against when there is no C compiler
# on the machine (D272). Run it inside an Alpine container, which is musl:
#
#   docker run --rm -v "$PWD:/src" -w /src alpine:3.22 \
#       sh tools/package/make_sysroot.sh build/sysroot
#
# What it writes under <out>:
#
#   bin/ld.lld                 LLVM's linker, from the pinned LLVM release
#   lib/crt1.o crti.o crtn.o   musl's start-up files
#   lib/libc.a                 musl
#   lib/libc++.a libc++abi.a libunwind.a
#   lib/libwolang_runtime.a    the runtime, built here against all of the above
#   MANIFEST                   the sha256 of each file, and the versions behind them
#
# A program linked this way is static, so it runs on any Linux with the same
# architecture, and the same source and flags give the same bytes: no build id,
# no timestamps. `wolang build` finds the directory through WOLANG_SYSROOT, or
# at `lib/wolang/sysroot` of an installed compiler.
set -eu

out=${1:?usage: make_sysroot.sh <out dir>}
llvm_version=${WOLANG_SYSROOT_LLVM:-22.1.8}
here=$(cd "$(dirname "$0")/../.." && pwd)

case $(uname -m) in
  x86_64) llvm_arch=X64 ;;
  aarch64) llvm_arch=ARM64 ;;
  *) echo "make_sysroot: no LLVM release for $(uname -m)" >&2; exit 1 ;;
esac

apk add --no-cache clang lld musl-dev libc++-dev libc++-static llvm-libunwind-static \
    cmake ninja curl xz tar coreutils

mkdir -p "$out/bin" "$out/lib"

# musl's own files.
for file in crt1.o crti.o crtn.o libc.a; do
  cp "$(find /usr/lib /usr/local/lib -name "$file" | head -n 1)" "$out/lib/$file"
done

# The C++ library the runtime was written against, built for musl.
for file in libc++.a libc++abi.a libunwind.a; do
  cp "$(find /usr/lib /usr/local/lib -name "$file" | head -n 1)" "$out/lib/$file"
done

# The runtime, built against the files above and nothing else.
build=$(mktemp -d)
CC=clang CXX="clang++ -stdlib=libc++" cmake -S "$here" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-stdlib=libc++ -ffile-prefix-map=$here=. -fdebug-prefix-map=$here=." \
    -DWOLANG_WITH_LLVM=OFF -DWOLANG_BUILD_TESTS=OFF -DWOLANG_BUILD_TOOLS=OFF \
    -DWOLANG_BUILD_BENCHMARKS=OFF -DWOLANG_BUILD_PLATFORM=OFF
cmake --build "$build" --target wolang_runtime
cp "$(find "$build" -name libwolang_runtime.a | head -n 1)" "$out/lib/libwolang_runtime.a"

# The linker: LLVM's own release, the version the compiler is built with.
archive="LLVM-$llvm_version-Linux-$llvm_arch.tar.xz"
curl -fsSL -o "$build/$archive" \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/$archive"
tar -xJf "$build/$archive" -C "$build" --wildcards '*/bin/ld.lld' '*/bin/lld'
cp "$(find "$build" -path '*/bin/lld' | head -n 1)" "$out/bin/lld"
ln -sf lld "$out/bin/ld.lld"

(
  cd "$out"
  echo "llvm $llvm_version"
  echo "alpine $(cat /etc/alpine-release)"
  echo "musl $(apk info -v musl-dev | head -n 1)"
  find bin lib -type f | sort | xargs sha256sum
) > "$out/MANIFEST"
cat "$out/MANIFEST"
