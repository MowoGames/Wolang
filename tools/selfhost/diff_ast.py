#!/usr/bin/env python3
"""Compares `wolang ast` (C++) with the Wolang parser, file by file (D283).

    python3 tools/selfhost/diff_ast.py [--wolang build/windows/tools/wolang/wolang.exe] [files...]

With no files it takes every tracked .wo file and every `wolang` block of the
guides. A file the C++ parser marks `has-error` in is counted and left out (the exit code is not used: a lone file also fails the checker on its imports): the
Wolang parser reads valid programs. Prints the first line where two dumps
differ for each file that does, and a summary.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

sys.path.insert(0, str(Path(__file__).resolve().parent))
import diff_tokens  # noqa: E402

ROOT = diff_tokens.ROOT
CHUNK = 40


def cpp_dump(driver, name):
    done = subprocess.run([driver, "ast", name], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if not done.stdout or "has-error" in done.stdout or re.search(r"error\[E00[0-5]\d\]", done.stderr):
        return None
    return done.stdout


def wolang_dumps(driver, names):
    done = subprocess.run([driver, "run", "tools/selfhost/syntax", "--", "ast"] + names, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if done.stderr.strip():
        print("stderr:", done.stderr.strip().splitlines()[-1][:200])
    files = {}
    current = None
    for line in done.stdout.split("\n"):
        if line.startswith("# "):
            current = line[2:]
            files[current] = []
        elif current is not None:
            files[current].append(line)
    return {name: "\n".join(lines).rstrip("\n") + "\n" for name, lines in files.items()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wolang", default=str(ROOT / "build/windows/tools/wolang/wolang.exe"))
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("files", nargs="*")
    options = parser.parse_args()

    files = options.files or diff_tokens.tracked() + diff_tokens.doc_blocks()
    expected = {}
    skipped = 0
    for name in files:
        dump = cpp_dump(options.wolang, name)
        if dump is None:
            skipped += 1
        else:
            expected[name] = dump

    names = list(expected)
    bad = 0
    for start in range(0, len(names), CHUNK):
        chunk = names[start:start + CHUNK]
        found = wolang_dumps(options.wolang, chunk)
        for name in chunk:
            want = expected[name]
            have = found.get(name)
            if have is None:
                bad += 1
                print(f"FAIL {name}: the Wolang parser printed nothing")
                continue
            if have == want:
                continue
            bad += 1
            a = want.split("\n")
            b = have.split("\n")
            at = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            if not options.quiet:
                print(f"FAIL {name}: line {at + 1}")
                print("  C++    :", a[at] if at < len(a) else "<end>")
                print("  Wolang :", b[at] if at < len(b) else "<end>")

    total = len(names)
    print(f"{total} files compared, {total - bad} identical, {bad} differ, {skipped} left out (the C++ parser reports an error)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
