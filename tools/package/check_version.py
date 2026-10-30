#!/usr/bin/env python3
"""Fails when the places that state the compiler's version disagree (D248).

    python3 tools/package/check_version.py                # all agree
    python3 tools/package/check_version.py --tag v1.1.0   # and match a tag
    python3 tools/package/check_version.py --print        # the version

CMakeLists.txt is the truth. Also requires CHANGELOG.md to have a section
for it, so a release cannot be tagged without notes.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def find(name, pattern):
    text = (ROOT / name).read_text(encoding="utf-8")
    match = re.search(pattern, text)
    return match.group(1) if match else None


def versions():
    manifest = json.loads(
        (ROOT / "editors/vscode/package.json").read_text(encoding="utf-8"))
    return {
        "CMakeLists.txt": find("CMakeLists.txt",
                               r"(?m)^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$"),
        "editors/vscode/package.json": manifest.get("version"),
        "editors/zed/extension.toml": find(
            "editors/zed/extension.toml", r'(?m)^version\s*=\s*"([^"]+)"'),
    }


def main(argv):
    found = versions()
    truth = found["CMakeLists.txt"]
    if truth is None:
        print("check_version: no VERSION in CMakeLists.txt",
              file=sys.stderr)
        return 1
    if argv[1:] == ["--print"]:
        print(truth)
        return 0
    bad = False
    for name, version in found.items():
        if version != truth:
            print(f"check_version: {name} says {version}, "
                  f"CMakeLists.txt says {truth}", file=sys.stderr)
            bad = True
    if argv[1:2] == ["--tag"]:
        if len(argv) != 3:
            print("usage: check_version.py --tag vX.Y.Z", file=sys.stderr)
            return 2
        if argv[2] != "v" + truth:
            print(f"check_version: the tag is {argv[2]} but the source "
                  f"says v{truth}", file=sys.stderr)
            bad = True
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    if not re.search(r"(?m)^## \[?" + re.escape(truth) + r"\b", changelog):
        print(f"check_version: CHANGELOG.md has no '## {truth}' section",
              file=sys.stderr)
        bad = True
    if bad:
        return 1
    print(f"version {truth}: all places agree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
