#!/usr/bin/env python3
"""Compares `wolang fetch` (C++) with tools/selfhost/fetch (Wolang) (D286).

    python3 tools/selfhost/diff_fetch.py [--wolang build/windows/tools/wolang/wolang.exe]

Builds local git repositories (one depending on another, a tag each), two
copies of the same application package that depend on them in several ways,
runs one tool in each copy and fails when the two `wolang.lock` files differ
byte for byte once the copy's own directory is replaced by a placeholder.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GIT = ["git", "-c", "user.name=wolang", "-c", "user.email=wolang@example.com"]


def run(args, cwd=None):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def repository(base, name, manifest, tag):
    path = base / "remote" / name
    write(path / "wolang.toml", manifest)
    write(path / "src" / f"{name}.wo", f"offer fun {name.capitalize()}() -> int => 1\n")
    run(GIT + ["init", "--quiet"], cwd=path)
    run(GIT + ["add", "."], cwd=path)
    run(GIT + ["commit", "--quiet", "-m", "first"], cwd=path)
    run(GIT + ["tag", tag], cwd=path)
    return str(path).replace("\\", "/")


def application(base, name, math, util):
    app = base / name / "app"
    write(app / "wolang.toml", f'''[package]
name = "app"
version = "0.1.0"

[dependencies]
core = {{ path = "../core" }}
util = {{ git = "{util}", tag = "v1" }}
math = {{ git = "{math}", tag = "v2" }}
''')
    write(app / "src" / "app.wo", "fun main() {}\n")
    write(base / name / "core" / "wolang.toml", f'''[package]
name = "core"
version = "2.0.0"

[dependencies]
math = {{ git = "{math}", tag = "v2" }}
''')
    write(base / name / "core" / "src" / "core.wo", "offer fun Core() -> int => 1\n")
    return app


def normal(text, app):
    for form in {str(app), str(app).replace("\\", "/"), str(app.resolve()), str(app.resolve()).replace("\\", "/")}:
        text = text.replace(form, "<APP>")
    return text.replace("\\", "/")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wolang", default=str(ROOT / "build/windows/tools/wolang/wolang.exe"))
    options = parser.parse_args()

    if shutil.which("git") is None:
        print("skip: git is not on the path")
        return 0

    base = Path(tempfile.mkdtemp(prefix="wofetch_"))
    try:
        math = repository(base, "math", '[package]\nname = "math"\nversion = "1.4.0"\n', "v2")
        util = repository(base, "util", f'[package]\nname = "util"\nversion = "0.9.0"\n\n[dependencies]\nmath = {{ git = "{math}", tag = "v2" }}\n', "v1")
        cpp = application(base, "cpp", math, util)
        wo = application(base, "wo", math, util)

        first = run([options.wolang, "fetch", str(cpp)], cwd=ROOT)
        second = run([options.wolang, "run", "tools/selfhost/fetch", "--", "fetch", str(wo)], cwd=ROOT)
        print("C++    :", first.stdout.strip().splitlines()[-1:] or first.stderr.strip()[-200:])
        print("Wolang :", second.stdout.strip().splitlines()[-1:] or second.stderr.strip()[-200:])

        lock_a = cpp / "wolang.lock"
        lock_b = wo / "wolang.lock"
        if not lock_a.exists() or not lock_b.exists():
            print("FAIL: a lock file is missing", "C++" if not lock_a.exists() else "", "Wolang" if not lock_b.exists() else "")
            return 1
        a = normal(lock_a.read_text(encoding="utf-8"), cpp)
        b = normal(lock_b.read_text(encoding="utf-8"), wo)
        a = a.replace("/cpp/", "/<COPY>/")
        b = b.replace("/wo/", "/<COPY>/")
        if a != b:
            print("FAIL: the lock files differ")
            import difflib
            for line in difflib.unified_diff(a.splitlines(), b.splitlines(), "C++", "Wolang", lineterm=""):
                print(line)
            return 1
        print(f"ok: {a.count('[[package]]')} packages locked, identical")
        return 0
    finally:
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
