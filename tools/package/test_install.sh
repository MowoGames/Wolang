#!/bin/sh
# Tests install.sh against a fake release (D248).
#
#   sh tools/package/test_install.sh [BUILD_DIR]     # default build/debug
#
# Lays out an archive the way release.yml's Assemble step does (cmake
# --install, then tar), serves it from localhost, and points the installer
# at it with WOLANG_DOWNLOAD_BASE. Checks that it installs and runs, and
# that it refuses a bad checksum, a missing one, and a bad version.

set -eu

here="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$here/build/debug}"
case "$build" in /*) ;; *) build="$here/$build" ;; esac
[ -x "$build/tools/wolang/wolang" ] || { echo "build $build first" >&2; exit 1; }

version="$(python3 "$here/tools/package/check_version.py" --print)"
case "$(uname -s)" in Linux) os=linux ;; Darwin) os=macos ;; *) exit 0 ;; esac
case "$(uname -m)" in
  x86_64|amd64) arch=x86_64 ;;
  arm64|aarch64) arch=arm64 ;;
  *) echo "no build for $(uname -m)" >&2; exit 1 ;;
esac
name="wolang-$os-$arch"

work="$(mktemp -d)"
server=""
cleanup() {
  [ -z "$server" ] || kill "$server" 2> /dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

# The release, as release.yml lays it out.
mkdir "$work/serve"
cmake --install "$build" --prefix "$work/serve/$name" > "$work/install.log" ||
  { cat "$work/install.log"; fail "cmake --install"; }
(cd "$work/serve" && tar -czf "$name.tar.gz" "$name" &&
  { sha256sum "$name.tar.gz" 2> /dev/null ||
    shasum -a 256 "$name.tar.gz"; } > SHA256SUMS)
rm -rf "$work/serve/$name"

# A free port, then the server.
port="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
python3 -m http.server "$port" --bind 127.0.0.1 --directory "$work/serve" \
  > "$work/server.log" 2>&1 &
server=$!
base="http://127.0.0.1:$port"
n=0
until curl -fs "$base/SHA256SUMS" > /dev/null 2>&1; do
  n=$((n + 1))
  [ "$n" -lt 50 ] || fail "the local server did not start"
  sleep 0.2
done

install() {
  # A home of its own, so no profile of the machine is read or written.
  HOME="$work/home" WOLANG_DOWNLOAD_BASE="$base" \
    sh "$here/install.sh" --no-modify-path --prefix "$work/w" "$@"
}
mkdir -p "$work/home"

# 1. The plain install, with and without the leading v.
for spelled in "$version" "v$version"; do
  rm -rf "$work/w"
  install --version "$spelled" > "$work/out.log" 2>&1 ||
    { cat "$work/out.log"; fail "install --version $spelled"; }
  grep -q "checksum matches" "$work/out.log" || fail "no checksum was checked"
  got="$("$work/w/bin/wolang" --version)"
  [ "$got" = "wolang $version" ] || fail "--version said '$got'"
  pass "install --version $spelled -> $got"
done
"$work/w/bin/wolang" run "$here/examples/hello.wo" > /dev/null ||
  fail "wolang run examples/hello.wo"
pass "wolang run examples/hello.wo"

# 2. Refusals. Each must fail, and leave nothing installed.
refuse() {
  what="$1"; shift
  rm -rf "$work/w"
  if install "$@" > "$work/out.log" 2>&1; then
    cat "$work/out.log"; fail "$what was installed"
  fi
  [ ! -e "$work/w/current" ] || fail "$what left files behind"
  pass "refuses $what"
}

good="$(cat "$work/serve/SHA256SUMS")"
zeros="$(printf '%064d' 0)"
printf '%s\n' "$good" | sed "s/^[0-9a-f]\{64\}/$zeros/" \
  > "$work/serve/SHA256SUMS"
refuse "a wrong checksum" --version "$version"

printf '%064d  other.tar.gz\n' 0 > "$work/serve/SHA256SUMS"
refuse "a SHA256SUMS with no line for the archive" --version "$version"

rm "$work/serve/SHA256SUMS"
refuse "a release with no SHA256SUMS" --version "$version"

refuse "a version that is not one" --version latest-ish

# 3. --no-verify is the explicit way past it.
rm -rf "$work/w"
install --version "$version" --no-verify > "$work/out.log" 2>&1 ||
  { cat "$work/out.log"; fail "install --no-verify"; }
[ -x "$work/w/bin/wolang" ] || fail "--no-verify installed nothing"
pass "--no-verify installs without SHA256SUMS"

# 4. Uninstall.
sh "$here/install.sh" --uninstall --prefix "$work/w" > /dev/null
[ ! -e "$work/w" ] || fail "--uninstall left the prefix"
pass "--uninstall"

echo "install.sh: all checks passed"
