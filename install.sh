#!/bin/sh
# Install Wolang.
#
#   curl -fsSL https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.sh | sh
#
# What it does: works out which build this machine wants, downloads it
# from the releases, checks it against the published checksum, unpacks it
# under ~/.wolang, and points ~/.wolang/bin at it. Nothing is written
# outside the prefix, and nothing needs a password (D185).
#
#   --version X.Y.Z   a particular release (v1.1.0 works too), not the newest
#   --prefix PATH     somewhere other than ~/.wolang
#   --from-source     clone and build instead of downloading
#   --uninstall       take it away again
#   --no-modify-path  do not touch the shell profile
#   --no-verify       install without checking SHA256SUMS (not advised)
#
# POSIX shell on purpose: this has to run on a machine where nothing is
# installed yet, which is the whole point of it.

set -eu

REPO="MowoGames/Wolang"
PREFIX="${WOLANG_HOME:-$HOME/.wolang}"
VERSION="${WOLANG_VERSION:-latest}"
FROM_SOURCE=0
UNINSTALL=0
MODIFY_PATH=1
VERIFY=1

say() { printf '%s\n' "$*"; }
die() { printf 'wolang: %s\n' "$*" >&2; exit 1; }
need() {
  command -v "$1" > /dev/null 2>&1 || die "this needs $1, which is not here"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2:?--version needs a number}"; shift 2 ;;
    --prefix) PREFIX="${2:?--prefix needs a path}"; shift 2 ;;
    --from-source) FROM_SOURCE=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    --no-modify-path) MODIFY_PATH=0; shift ;;
    --no-verify) VERIFY=0; shift ;;
    -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1" ;;
  esac
done

if [ "$UNINSTALL" = 1 ]; then
  [ -d "$PREFIX" ] || die "nothing installed at $PREFIX"
  rm -rf "$PREFIX"
  say "removed $PREFIX"
  say "the line it added to your shell profile is still there; take it out"
  exit 0
fi

# ------------------------------------------------------- this machine ---

case "$(uname -s)" in
  Linux) OS=linux ;;
  Darwin) OS=macos ;;
  MINGW*|MSYS*|CYGWIN*)
    die "this is for Unix; on Windows run install.ps1 in PowerShell" ;;
  *) die "no build for $(uname -s)" ;;
esac

case "$(uname -m)" in
  x86_64|amd64) ARCH=x86_64 ;;
  arm64|aarch64) ARCH=arm64 ;;
  *) die "no build for $(uname -m); try --from-source" ;;
esac

TARGET="wolang-$OS-$ARCH"

# ------------------------------------------------------- from source ---

if [ "$FROM_SOURCE" = 1 ]; then
  need git
  need cmake
  # This project is built with clang (D184).
  if [ -z "${CXX:-}" ]; then
    if command -v clang++ > /dev/null 2>&1; then
      CXX=clang++
      CC=clang
      export CC CXX
    elif [ "$OS" = macos ]; then
      : # Apple's `c++` is clang.
    else
      die "clang is needed to build this; install clang and try again"
    fi
  fi
  work="$(mktemp -d)"
  say "cloning into $work"
  git clone --depth 1 "https://github.com/$REPO" "$work/wolang"
  cd "$work/wolang"
  cmake --preset release
  cmake --build --preset release -j
  cmake --install build/release --prefix "$PREFIX"
  say "built and installed into $PREFIX"
else
  # ----------------------------------------------------- from a release ---
  need curl
  need tar

  if [ "$VERSION" = latest ]; then
    say "looking up the newest release"
    # The API first; it is rate limited (60 an hour without a token), so
    # when it refuses, follow the /releases/latest redirect, which is not.
    VERSION="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
                 2> /dev/null |
               sed -n 's/.*"tag_name" *: *"v\{0,1\}\([^"]*\)".*/\1/p' |
               head -1)" || VERSION=""
    if [ -z "$VERSION" ]; then
      final="$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
                 "https://github.com/$REPO/releases/latest" 2> /dev/null)" ||
        final=""
      VERSION="$(printf '%s\n' "$final" |
                 sed -n 's|.*/releases/tag/v\{0,1\}\([^/]*\)$|\1|p')"
    fi
    [ -n "$VERSION" ] || die "could not find a release; try --from-source"
  fi
  # v1.1.0 and 1.1.0 are the same request.
  VERSION="${VERSION#v}"
  case "$VERSION" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "'$VERSION' is not a version like 1.1.0" ;;
  esac

  # WOLANG_DOWNLOAD_BASE is for tools/package/test_install.sh only: it
  # points the download at a local server instead of GitHub (D248).
  base="${WOLANG_DOWNLOAD_BASE:-https://github.com/$REPO/releases/download/v$VERSION}"
  archive="$TARGET.tar.gz"
  work="$(mktemp -d)"
  trap 'rm -rf "$work"' EXIT

  say "downloading $archive ($VERSION)"
  curl -fsSL "$base/$archive" -o "$work/$archive" ||
    die "no build for $TARGET in v$VERSION; try --from-source"

  # A download nobody checked is a download somebody else could have
  # written, so a missing checksum is a failure, not a shrug.
  if [ "$VERIFY" = 1 ]; then
    curl -fsSL "$base/SHA256SUMS" -o "$work/SHA256SUMS" ||
      die "v$VERSION published no SHA256SUMS; refusing to install (--no-verify skips the check)"
    expected="$(sed -n "s/^\([0-9a-fA-F]\{64\}\) [ *]\(\.\/\)\{0,1\}$archive\$/\1/p" \
                "$work/SHA256SUMS" | head -1)"
    [ -n "$expected" ] ||
      die "SHA256SUMS has no line for $archive; refusing to install (--no-verify skips the check)"
    if command -v sha256sum > /dev/null 2>&1; then
      actual="$(sha256sum "$work/$archive" | cut -d' ' -f1)"
    elif command -v shasum > /dev/null 2>&1; then
      actual="$(shasum -a 256 "$work/$archive" | cut -d' ' -f1)"
    else
      die "this needs sha256sum or shasum to check the download"
    fi
    [ "$actual" = "$expected" ] ||
      die "the checksum does not match; refusing to install"
    say "checksum matches"
  else
    say "skipping the checksum (--no-verify)"
  fi

  # The archive's one top-level directory is named after the platform
  # today; read it rather than assume, so a renamed one still installs.
  mkdir "$work/x"
  tar -xzf "$work/$archive" -C "$work/x"
  top="$(tar -tzf "$work/$archive" | sed -n '1s|^\./||;1s|/.*||p')"
  [ -n "$top" ] && [ -x "$work/x/$top/bin/wolang" ] ||
    die "the archive does not have bin/wolang in its top directory"
  mkdir -p "$PREFIX"
  rm -rf "$PREFIX/current"
  mv "$work/x/$top" "$PREFIX/current"
  mkdir -p "$PREFIX/bin"
  ln -sf "$PREFIX/current/bin/wolang" "$PREFIX/bin/wolang"
  say "installed $VERSION into $PREFIX"
fi

# --------------------------------------------------------------- path ---

if [ "$MODIFY_PATH" = 1 ]; then
  line="export PATH=\"$PREFIX/bin:\$PATH\""
  for profile in "$HOME/.zshrc" "$HOME/.bashrc" "$HOME/.profile"; do
    [ -f "$profile" ] || continue
    if ! grep -qs "$PREFIX/bin" "$profile"; then
      printf '\n# Added by the Wolang installer\n%s\n' "$line" >> "$profile"
      say "added $PREFIX/bin to your PATH in $profile"
    fi
  done
fi

say ""
if [ -x "$PREFIX/bin/wolang" ]; then
  "$PREFIX/bin/wolang" --version || true
fi
say ""
say "Try it:"
say "  export PATH=\"$PREFIX/bin:\$PATH\""
say "  wolang new hello && cd hello && wolang run ."
