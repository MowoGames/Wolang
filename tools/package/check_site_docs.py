#!/usr/bin/env python3
"""Checks that the documentation converts into pages the website can build.

The site renders MDX, where a stray `{` or `<` in prose is a build error, a
link to a page that is not there is a dead link, and a heading that moved
breaks every anchor that named it. `make_site_docs.py --check` converts
everything into a scratch directory and says which of these it found, so the
publish job never meets them on alpha (D210).

    python3 tools/package/check_site_docs.py
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def driver():
    for build in ("debug", "release", "windows"):
        held = ROOT / "build" / build / "tools" / "wolang" / "wolang"
        for candidate in (held, held.with_suffix(".exe")):
            if candidate.exists():
                return candidate
    return None


def main():
    command = [sys.executable, str(ROOT / "tools" / "package" / "make_site_docs.py"), "--check"]
    wolang = driver()
    if wolang is not None:
        command += ["--wolang", str(wolang)]
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main())
