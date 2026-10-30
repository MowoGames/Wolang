#!/usr/bin/env python3
"""Checks that the examples in the documentation compile.

A manual full of programs that do not build is worse than no manual: it
teaches the wrong thing and nobody finds out. So every fenced `wolang`
block that looks like a whole program -- one with a `fun main` in it --
is written into a package and handed to `wolang check` (D183).

    python3 tools/package/check_docs.py                 # every doc
    python3 tools/package/check_docs.py docs/stdlib.md  # one of them
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BLOCK = re.compile(r"```wolang\n(.*?)```", re.DOTALL)

MANIFEST = """[package]
name = "doccheck"
source = "src"

[dependencies]
std = {{ path = "{root}/lib/std" }}
http = {{ path = "{root}/lib/http" }}
db = {{ path = "{root}/lib/db" }}
gfx = {{ path = "{root}/lib/gfx" }}
image = {{ path = "{root}/lib/image" }}
game = {{ path = "{root}/lib/game" }}
"""


def driver():
    for build in ("debug", "release", "windows"):
        held = ROOT / "build" / build / "tools" / "wolang" / "wolang"
        if held.exists():
            return held
        held = held.with_suffix(".exe")
        if held.exists():
            return held
    return None


def check(path, wolang, verbose):
    path = path.resolve()
    shown = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
    text = path.read_text(encoding="utf-8")
    failed = 0
    checked = 0
    for index, block in enumerate(BLOCK.findall(text)):
        # A fragment is not a program: what is checked is what claims to
        # be one.
        if "fun main" not in block:
            continue
        checked += 1
        with tempfile.TemporaryDirectory() as held:
            package = Path(held)
            (package / "src").mkdir()
            (package / "wolang.toml").write_text(
                MANIFEST.format(root=ROOT.as_posix()), encoding="utf-8")
            (package / "src" / "main.wo").write_text(block,
                                                     encoding="utf-8")
            result = subprocess.run(
                [str(wolang), "check", str(package)],
                capture_output=True, text=True)
            if result.returncode != 0:
                failed += 1
                print("{}: block {} does not compile".format(
                    shown, index + 1))
                for line in (result.stdout + result.stderr).splitlines():
                    if line.strip():
                        print("    " + line)
            elif verbose:
                print("{}: block {} is fine".format(shown, index + 1))
    return checked, failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*",
                        help="the documents to check")
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args()

    wolang = driver()
    if wolang is None:
        print("no wolang driver built; run cmake --build --preset debug",
              file=sys.stderr)
        return 2

    paths = [Path(p) for p in arguments.paths]
    if not paths:
        paths = sorted((ROOT / "docs").glob("*.md"))
        paths.append(ROOT / "README.md")

    checked = 0
    failed = 0
    for path in paths:
        if not path.exists():
            continue
        one, bad = check(path, wolang, arguments.verbose)
        checked += one
        failed += bad
    print("{} programs in the documentation, {} broken".format(
        checked, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
