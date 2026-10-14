#!/bin/sh
# Runs the gameplay microbenchmarks in both languages and prints the ratios.
#
#   benchmarks/run.sh [path-to-wolang]
#
# Two C++ columns, because one of them answers a different question. `-O2`
# is what a gameplay build uses, and on a counting loop it produces vector
# code -- which a scalar dev tier cannot be within 3x of and is not trying
# to be. The `scalar` column is the same build with vectorization off, which
# is what "how good is the code this tier emits" actually compares against.
#
# Build the release preset first: timing a debug build measures the
# assertions.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
wolang=${1:-$root/build/release/tools/wolang/wolang}
work=$(mktemp -d -t wolang-bench)

if [ ! -x "$wolang" ]; then
  echo "no wolang at $wolang; run: cmake --preset release && cmake --build build/release" >&2
  exit 2
fi

c++ -std=c++20 -O2 -o "$work/vector" "$root/benchmarks/native/gameplay.cc"
c++ -std=c++20 -O2 -fno-vectorize -fno-slp-vectorize \
    -o "$work/scalar" "$root/benchmarks/native/gameplay.cc"

"$wolang" bench "$root/benchmarks/gameplay.wo" > "$work/wolang.txt"
"$work/vector" > "$work/vector.txt"
"$work/scalar" > "$work/scalar.txt"

printf '%-24s %11s %11s %7s %11s %7s\n' \
    benchmark wolang 'c++ -O2' ratio 'c++ scalar' ratio
awk '
  FILENAME == wolang_file { ms[$1] = $2; value[$1] = $5; next }
  FILENAME == vector_file { vec[$1] = $2; next }
  {
    name = $1
    if (!(name in ms)) next
    printf "%-24s %8.3f ms %8.3f ms %6.2fx %8.3f ms %6.2fx\n",
           name, ms[name], vec[name], ms[name] / (vec[name] + 1e-9),
           $2, ms[name] / ($2 + 1e-9)
  }
' wolang_file="$work/wolang.txt" vector_file="$work/vector.txt" \
  "$work/wolang.txt" "$work/vector.txt" "$work/scalar.txt"

rm -rf "$work"
