#!/usr/bin/env python3
"""Sets the compiler's version everywhere it is written (D248).

    python3 tools/package/bump_version.py 1.1.0

CMakeLists.txt `project(VERSION)` is the source of truth; the C++ gets it
from there at build time. What this edits are the places that cannot:
the editor extensions' manifests. `check_version.py` fails when they
disagree. Library package versions (lib/*/wolang.toml) are the packages'
own and are left alone.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (file, pattern with one group before the version, what it is)
PLACES = [
    ("CMakeLists.txt",
     r"(?m)^(\s*VERSION\s+)\d+\.\d+\.\d+(?=\s*$)", "CMake project"),
    ("editors/vscode/package.json",
     r'(?m)^(  "version":\s*")\d+\.\d+\.\d+(?=")', "VS Code extension"),
    ("editors/zed/extension.toml",
     r'(?m)^(version\s*=\s*")\d+\.\d+\.\d+(?=")', "Zed extension"),
]


def main(argv):
    if len(argv) != 2 or not re.fullmatch(r"\d+\.\d+\.\d+", argv[1]):
        print("usage: bump_version.py X.Y.Z", file=sys.stderr)
        return 2
    version = argv[1]
    for name, pattern, what in PLACES:
        path = ROOT / name
        text = path.read_text(encoding="utf-8")
        new, count = re.subn(pattern, lambda m: m.group(1) + version, text,
                             count=1)
        if count != 1:
            print(f"bump_version: no version found in {name}",
                  file=sys.stderr)
            return 1
        path.write_text(new, encoding="utf-8")
        print(f"{what}: {name} -> {version}")
    print("Now add a section for it to CHANGELOG.md, then run "
          "tools/package/check_version.py.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
