#!/usr/bin/env python3
"""Writes tools/selfhost/syntax/src/kinds.wo from token_kind.inc (D283).

    python3 tools/selfhost/gen_kinds.py          # rewrite the file
    python3 tools/selfhost/gen_kinds.py --check  # fail when it is stale

The C++ lexer and the Wolang lexer must name the same token kinds in the same
order, so the Wolang side is generated and never edited by hand.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / "include/wolang/syntax/token_kind.inc"
OUT = ROOT / "tools/selfhost/syntax/src/kinds.wo"

ENTRY = re.compile(r'^WOLANG_(TOKEN|PUNCT|KEYWORD|CONTEXTUAL|RESERVED)\((.*)\)\s*$')


def parse():
    tokens = []
    keywords = []
    reserved = []
    for line in INC.read_text(encoding="utf-8").splitlines():
        match = ENTRY.match(line.strip())
        if not match:
            continue
        macro, rest = match.groups()
        if macro == "RESERVED":
            reserved.append(rest.strip().strip('"'))
            continue
        if macro == "CONTEXTUAL":
            continue
        name, spelling = [part.strip() for part in rest.split(",", 1)]
        spelling = spelling.strip('"')
        tokens.append((name, spelling))
        if macro == "KEYWORD":
            keywords.append((name, spelling))
    return tokens, keywords, reserved


def render():
    tokens, keywords, reserved = parse()
    out = []
    out.append("need std.map.{Map, TextMapNew}")
    out.append("")
    out.append("offer enum Kind {")
    for name, _ in tokens:
        out.append("  " + name)
    out.append("}")
    out.append("")
    out.append("offer fun KindName(kind: Kind) -> text {")
    out.append("  return match kind {")
    for name, _ in tokens:
        out.append('    Kind.%s => "%s"' % (name, name))
    out.append("  }")
    out.append("}")
    out.append("")
    out.append("offer fun KeywordKinds() -> Map<text, Kind> uses unsafe {")
    out.append("  var table = TextMapNew<Kind>()")
    for name, spelling in keywords:
        out.append('  table.Put("%s", Kind.%s)' % (spelling, name))
    out.append("  return table")
    out.append("}")
    out.append("")
    out.append("offer fun ReservedWords() -> Map<text, bool> uses unsafe {")
    out.append("  var words = TextMapNew<bool>()")
    for spelling in reserved:
        out.append('  words.Put("%s", true)' % spelling)
    out.append("  return words")
    out.append("}")
    out.append("")
    return "\n".join(out)


def main(argv):
    text = render()
    if "--check" in argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current.replace("\r\n", "\n") != text:
            print("kinds.wo is stale: run tools/selfhost/gen_kinds.py", file=sys.stderr)
            return 1
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
