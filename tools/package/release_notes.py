#!/usr/bin/env python3
"""Prints the CHANGELOG.md section of one version, for the release body (D248).

    python3 tools/package/release_notes.py 1.1.0 > notes.md

Prints nothing and exits 1 when there is no such section; release.yml
then falls back to the notes GitHub generates.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def section(text, version):
    lines = text.splitlines()
    heading = re.compile(r"^## \[?" + re.escape(version) + r"\b")
    for start, line in enumerate(lines):
        if heading.match(line):
            break
    else:
        return None
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i].startswith("## "):
            end = i
            break
    # The heading is the release's title already; the body starts under it.
    return "\n".join(lines[start + 1:end]).strip() + "\n"


def main(argv):
    if len(argv) != 2:
        print("usage: release_notes.py X.Y.Z", file=sys.stderr)
        return 2
    body = section((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"),
                   argv[1].lstrip("v"))
    if body is None:
        print(f"release_notes: no section for {argv[1]}", file=sys.stderr)
        return 1
    sys.stdout.write(body)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
