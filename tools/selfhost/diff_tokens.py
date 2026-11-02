#!/usr/bin/env python3
"""Compares the C++ lexer with the Wolang lexer on every .wo file (D283).

    python3 tools/selfhost/diff_tokens.py [--wolang build/windows/tools/wolang/wolang.exe] [files...]

Both print `Kind offset length flags` per token; this fails when any file
differs. With no files it takes every tracked .wo file in the repository.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKIP = {"examples/errors.wo", "examples/recovery.wo"}
CHUNK = 60


def tracked():
    out = subprocess.check_output(["git", "ls-files", "*.wo"], cwd=ROOT, text=True)
    return [f for f in out.split() if f not in SKIP]


def doc_blocks():
    out = ROOT / "build/scratch/doc_blocks"
    out.mkdir(parents=True, exist_ok=True)
    names = []
    for doc in sorted((ROOT / "docs").glob("*.md")) + [ROOT / "README.md"]:
        if not doc.exists():
            continue
        text = doc.read_text(encoding="utf-8", errors="replace")
        block = None
        count = 0
        for line in text.splitlines():
            if block is None:
                if line.startswith("```wolang"):
                    block = []
            elif line.startswith("```"):
                count += 1
                path = out / f"{doc.stem}_{count}.wo"
                path.write_text("\n".join(block) + "\n", encoding="utf-8", newline="\n")
                names.append(path.relative_to(ROOT).as_posix())
                block = None
            else:
                block.append(line)
    return names


def split(text):
    files = {}
    current = None
    for line in text.splitlines():
        if line.startswith("# "):
            current = line[2:]
            files[current] = []
        elif current is not None and line:
            files[current].append(line)
    return files


def run(driver, args):
    done = subprocess.run([driver] + args, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return split(done.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wolang", default=str(ROOT / "build/windows/tools/wolang/wolang.exe"))
    parser.add_argument("files", nargs="*")
    options = parser.parse_args()

    files = options.files or tracked() + doc_blocks()
    bad = 0
    checked = 0
    total = 0
    for start in range(0, len(files), CHUNK):
        chunk = files[start:start + CHUNK]
        expected = run(options.wolang, ["tokens", "--stable"] + chunk)
        found = run(options.wolang, ["run", "tools/selfhost/syntax", "--", "tokens"] + chunk)
        for name in chunk:
            want = expected.get(name)
            have = found.get(name)
            checked += 1
            total += len(want or [])
            if want is None:
                print(f"skip {name}: the C++ lexer printed nothing")
                continue
            if have is None:
                print(f"FAIL {name}: the Wolang lexer printed nothing")
                bad += 1
                continue
            if want == have:
                continue
            bad += 1
            at = next((i for i, (a, b) in enumerate(zip(want, have)) if a != b), min(len(want), len(have)))
            print(f"FAIL {name}: token {at}: C++ {want[at] if at < len(want) else '<end>'!r} vs Wolang {have[at] if at < len(have) else '<end>'!r} ({len(want)} vs {len(have)} tokens)")

    print(f"{checked} files, {total} tokens, {bad} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
