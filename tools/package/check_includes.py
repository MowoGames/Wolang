#!/usr/bin/env python3
"""Checks that every file says what it needs.

A header that uses `std::string` without including `<string>` builds on
a library that drags it in anyway and fails on one that does not -- which
is how Windows found three dozen of them at once. This is the same
question asked on every commit instead (D189).

    python3 tools/package/check_includes.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# What each standard name needs. Not every name in the library: the ones
# this project uses, which is what a check has to cover to be worth
# running.
QUALIFIED = {
    "string": "<string>",
    "string_view": "<string_view>",
    "vector": "<vector>",
    "span": "<span>",
    "optional": "<optional>",
    "function": "<functional>",
    "array": "<array>",
    "unique_ptr": "<memory>",
    "make_unique": "<memory>",
    "shared_ptr": "<memory>",
    "unordered_map": "<unordered_map>",
    "unordered_set": "<unordered_set>",
    "map": "<map>",
    "set": "<set>",
    "pair": "<utility>",
    "mutex": "<mutex>",
    "lock_guard": "<mutex>",
    "atomic": "<atomic>",
    "thread": "<thread>",
    "sort": "<algorithm>",
    "memcpy": "<cstring>",
    "memset": "<cstring>",
}
PLAIN = {
    "uint8_t": "<cstdint>",
    "uint16_t": "<cstdint>",
    "uint32_t": "<cstdint>",
    "uint64_t": "<cstdint>",
    "int8_t": "<cstdint>",
    "int16_t": "<cstdint>",
    "int32_t": "<cstdint>",
    "int64_t": "<cstdint>",
    "size_t": "<cstddef>",
}


def includes_of(path):
    text = path.read_text(encoding="utf-8")
    return set(re.findall(r"#include\s+(<[^>]+>)", text))


def own_header(path):
    """The header a source file is the implementation of."""
    if path.suffix != ".cc":
        return None
    relative = Path(*path.relative_to(ROOT).parts[1:]).with_suffix(".h")
    held = ROOT / "include" / "wolang" / relative
    return held if held.exists() else None


def check(path):
    text = path.read_text(encoding="utf-8")
    body = re.sub(r"//[^\n]*", "", text)
    have = includes_of(path)
    header = own_header(path)
    if header is not None:
        have |= includes_of(header)

    missing = set()
    for name, wanted in QUALIFIED.items():
        if re.search(r"\bstd::" + name + r"\b", body) and wanted not in have:
            missing.add(wanted)
    for name, wanted in PLAIN.items():
        if re.search(r"\b" + name + r"\b", body) and wanted not in have:
            missing.add(wanted)
    return sorted(missing)


def main():
    bad = 0
    files = sorted(
        list((ROOT / "include").rglob("*.h")) +
        list((ROOT / "src").rglob("*.cc")) +
        list((ROOT / "tools").rglob("*.cc")) +
        list((ROOT / "platform").glob("*.cc")))
    for path in files:
        for wanted in check(path):
            print("{}: uses something from {} and does not include it"
                  .format(path.relative_to(ROOT), wanted))
            bad += 1
    print("{} files checked, {} missing includes".format(len(files), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
