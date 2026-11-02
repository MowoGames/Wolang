#!/usr/bin/env python3
"""Writes lib/font/src/fixtures.wo and lib/font/src/sample.wo (D276).

    python tools/gen/make_font_fixtures.py

fixtures.wo holds two tiny TrueType fonts the tests parse: one with short
`loca`, a `cmap` format 4, a `kern` pair and a composite glyph; one with
long `loca`, a `cmap` format 12 and metrics shared past the last full
entry. sample.wo holds a complete one-case 5x7 font drawn from rectangles,
so a program can put text on screen with no font file.
"""

import base64
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def u16(v):
    return struct.pack(">H", v & 0xFFFF)


def i16(v):
    return struct.pack(">h", v)


def u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def pad4(data):
    return data + b"\0" * ((4 - len(data) % 4) % 4)


def checksum(data):
    data = pad4(data)
    total = 0
    for i in range(0, len(data), 4):
        total = (total + struct.unpack(">I", data[i:i + 4])[0]) & 0xFFFFFFFF
    return total


def simple_glyph(contours):
    points = [p for c in contours for p in c]
    if not points:
        return b""
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    out = i16(len(contours)) + i16(min(xs)) + i16(min(ys)) + i16(max(xs)) + i16(max(ys))
    end = -1
    for c in contours:
        end += len(c)
        out += u16(end)
    out += u16(0)
    out += bytes(1 if p[2] else 0 for p in points)
    px = 0
    for p in points:
        out += i16(p[0] - px)
        px = p[0]
    py = 0
    for p in points:
        out += i16(p[1] - py)
        py = p[1]
    return out


def composite_glyph(parts, bbox):
    out = i16(-1) + i16(bbox[0]) + i16(bbox[1]) + i16(bbox[2]) + i16(bbox[3])
    for index, (glyph, dx, dy, scale) in enumerate(parts):
        flags = 0x0001 | 0x0002
        if scale is not None:
            flags |= 0x0008
        if index != len(parts) - 1:
            flags |= 0x0020
        out += u16(flags) + u16(glyph) + i16(dx) + i16(dy)
        if scale is not None:
            out += i16(int(round(scale * 16384)))
    return out


def cmap_format4(segments):
    segs = sorted(segments) + [(0xFFFF, 0xFFFF, 0)]
    count = len(segs)
    pow2 = 1
    log = 0
    while pow2 * 2 <= count:
        pow2 *= 2
        log += 1
    body = u16(4) + u16(0) + u16(0) + u16(count * 2)
    body += u16(pow2 * 2) + u16(log) + u16(count * 2 - pow2 * 2)
    body += b"".join(u16(s[1]) for s in segs)
    body += u16(0)
    body += b"".join(u16(s[0]) for s in segs)
    body += b"".join(u16(s[2] - s[0] if s[0] != 0xFFFF else 1) for s in segs)
    body += b"".join(u16(0) for _ in segs)
    return body[:2] + u16(len(body)) + body[4:]


def cmap_format12(groups):
    body = u16(12) + u16(0) + u32(16 + 12 * len(groups)) + u32(0) + u32(len(groups))
    for start, end, glyph in sorted(groups):
        body += u32(start) + u32(end) + u32(glyph)
    return body


def cmap_table(subtables):
    head = u16(0) + u16(len(subtables))
    offset = 4 + 8 * len(subtables)
    records = b""
    bodies = b""
    for platform, encoding, body in subtables:
        records += u16(platform) + u16(encoding) + u32(offset + len(bodies))
        bodies += body
    return head + records + bodies


def kern_table(pairs):
    pairs = sorted(pairs)
    count = len(pairs)
    pow2 = 1
    log = 0
    while pow2 * 2 <= count:
        pow2 *= 2
        log += 1
    sub = u16(0) + u16(14 + 6 * count) + u16(1) + u16(count)
    sub += u16(pow2 * 6) + u16(log) + u16(count * 6 - pow2 * 6)
    for left, right, value in pairs:
        sub += u16(left) + u16(right) + i16(value)
    return u16(0) + u16(1) + sub


def build_font(units, ascent, descent, gap, glyphs, advances, lsbs, h_metrics,
               cmap, kern, long_loca):
    count = len(glyphs)
    glyf = b""
    offsets = []
    for g in glyphs:
        offsets.append(len(glyf))
        glyf += pad4(g)
    offsets.append(len(glyf))
    if long_loca:
        loca = b"".join(u32(o) for o in offsets)
    else:
        loca = b"".join(u16(o // 2) for o in offsets)
    xs = [advances[i] for i in range(count)]
    head = u32(0x00010000) + u32(0) + u32(0) + u32(0x5F0F3CF5) + u16(0)
    head += u16(units) + b"\0" * 16 + i16(0) + i16(descent) + i16(max(xs)) + i16(ascent)
    head += u16(0) + u16(8) + i16(2) + i16(1 if long_loca else 0) + i16(0)
    maxp = u32(0x00010000) + u16(count) + b"\0" * 26
    hhea = u32(0x00010000) + i16(ascent) + i16(descent) + i16(gap) + u16(max(xs))
    hhea += i16(0) + i16(0) + i16(max(xs)) + i16(1) + i16(0) + i16(0)
    hhea += b"\0" * 8 + i16(0) + u16(h_metrics)
    hmtx = b""
    for i in range(count):
        if i < h_metrics:
            hmtx += u16(advances[i]) + i16(lsbs[i])
        else:
            hmtx += i16(lsbs[i])
    tables = {
        b"cmap": cmap,
        b"glyf": glyf,
        b"head": head,
        b"hhea": hhea,
        b"hmtx": hmtx,
        b"loca": loca,
        b"maxp": maxp,
    }
    if kern is not None:
        tables[b"kern"] = kern
    names = sorted(tables)
    n = len(names)
    pow2 = 1
    log = 0
    while pow2 * 2 <= n:
        pow2 *= 2
        log += 1
    out = u32(0x00010000) + u16(n) + u16(pow2 * 16) + u16(log) + u16(n * 16 - pow2 * 16)
    offset = 12 + 16 * n
    body = b""
    for name in names:
        data = tables[name]
        out += name + u32(checksum(data)) + u32(offset + len(body)) + u32(len(data))
        body += pad4(data)
    return out + body


def tiny_font_a():
    notdef = simple_glyph([[(100, 0, True), (500, 0, True), (500, 700, True), (100, 700, True)]])
    space = b""
    letter_a = simple_glyph([[(0, 0, True), (250, 800, False), (500, 0, True)]])
    letter_b = composite_glyph([(2, 0, 0, None), (2, 300, 0, 0.5)], (0, 0, 650, 800))
    glyphs = [notdef, space, letter_a, letter_b]
    advances = [600, 300, 600, 800]
    lsbs = [100, 0, 0, 0]
    cmap = cmap_table([(3, 1, cmap_format4([(32, 32, 1), (65, 66, 2)]))])
    kern = kern_table([(2, 3, -50)])
    return build_font(1000, 800, -200, 100, glyphs, advances, lsbs, 4, cmap, kern, False)


def tiny_font_b():
    notdef = simple_glyph([[(100, 0, True), (500, 0, True), (500, 700, True), (100, 700, True)]])
    box = simple_glyph([
        [(0, 0, True), (400, 0, True), (400, 400, True), (0, 400, True)],
        [(100, 100, True), (100, 300, True), (300, 300, True), (300, 100, True)],
    ])
    dot = simple_glyph([[(0, 0, True), (100, 0, True), (100, 100, True), (0, 100, True)]])
    glyphs = [notdef, box, dot]
    advances = [600, 500, 500]
    lsbs = [100, 0, 0]
    cmap = cmap_table([(3, 10, cmap_format12([(0x1F600, 0x1F600, 1), (0x2E, 0x2E, 2)]))])
    return build_font(2048, 1600, -400, 0, glyphs, advances, lsbs, 2, cmap, None, True)


FONT_5X7 = {
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["####.", "....#", "....#", ".###.", "....#", "....#", "####."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    ".": [".....", ".....", ".....", ".....", ".....", ".....", "..#.."],
    ",": [".....", ".....", ".....", ".....", ".....", "..#..", ".#..."],
    ":": [".....", ".....", "..#..", ".....", ".....", "..#..", "....."],
    ";": [".....", ".....", "..#..", ".....", ".....", "..#..", ".#..."],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    "+": [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."],
    "=": [".....", ".....", "#####", ".....", "#####", ".....", "....."],
    "/": ["....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."],
    "(": ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."],
    ")": [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."],
    "_": [".....", ".....", ".....", ".....", ".....", ".....", "#####"],
    "'": ["..#..", "..#..", ".....", ".....", ".....", ".....", "....."],
    '"': [".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."],
    "*": [".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", "....."],
    "%": ["##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"],
    "#": [".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."],
    "<": ["...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."],
    ">": [".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."],
    "[": [".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."],
    "]": [".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."],
}


def rects_of(rows):
    out = []
    for r, row in enumerate(rows):
        y_top = 7 - r
        c = 0
        while c < 5:
            if row[c] != "#":
                c += 1
                continue
            start = c
            while c < 5 and row[c] == "#":
                c += 1
            out.append((start, y_top - 1, c, y_top))
    return out


def rect_contour(x0, y0, x1, y1):
    return [(x0 * 2, y0 * 2, True), (x0 * 2, y1 * 2, True), (x1 * 2, y1 * 2, True), (x1 * 2, y0 * 2, True)]


def sample_font():
    keys = sorted(FONT_5X7)
    glyph_of = {}
    glyphs = []
    notdef = simple_glyph([
        [(0, 0, True), (0, 14, True), (10, 14, True), (10, 0, True)],
        [(2, 2, True), (8, 2, True), (8, 12, True), (2, 12, True)],
    ])
    glyphs.append(notdef)
    for ch in keys:
        glyph_of[ch] = len(glyphs)
        contours = [rect_contour(*r) for r in rects_of(FONT_5X7[ch])]
        glyphs.append(simple_glyph(contours))
    space_glyph = len(glyphs)
    glyphs.append(b"")
    codes = {}
    for ch in keys:
        codes[ord(ch)] = glyph_of[ch]
        if ch.isalpha():
            codes[ord(ch.lower())] = glyph_of[ch]
    codes[32] = space_glyph
    advances = [12] * len(glyphs)
    advances[space_glyph] = 8
    lsbs = [0] * len(glyphs)
    segments = []
    for code in sorted(codes):
        glyph = codes[code]
        if segments and segments[-1][1] + 1 == code and segments[-1][2] + (code - segments[-1][0]) == glyph:
            segments[-1] = (segments[-1][0], code, segments[-1][2])
        else:
            segments.append((code, code, glyph))
    cmap = cmap_table([(3, 1, cmap_format4(segments))])
    return build_font(16, 14, -2, 2, glyphs, advances, lsbs, len(glyphs), cmap, None, False)


def emit_function(name, data, chunk=3000):
    text = base64.b64encode(data).decode("ascii")
    pieces = [text[i:i + chunk] for i in range(0, len(text), chunk)]
    out = []
    for index, piece in enumerate(pieces):
        out.append(f"fun {name}Part{index}() -> text => \"{piece}\"\n")
    return out, len(pieces)


def write_fixtures():
    lines = []
    for name, data in (("TinyFontA", tiny_font_a()), ("TinyFontB", tiny_font_b())):
        funs, count = emit_function(name, data)
        for f in funs:
            lines.append(f + "\n")
        body = " + ".join(f"{name}Part{i}()" for i in range(count))
        lines.append(f"fun {name}Base64() -> text => {body}\n\n")
    (ROOT / "lib/font/src/fixtures.wo").write_text("".join(lines), encoding="utf-8", newline="\n")


def write_sample():
    data = sample_font()
    funs, count = emit_function("SampleFont", data)
    lines = []
    for f in funs:
        lines.append(f + "\n")
    body = " + ".join(f"SampleFontPart{i}()" for i in range(count))
    lines.append(f"fun SampleFontBase64() -> text => {body}\n")
    (ROOT / "lib/font/src/sample.wo").write_text("".join(lines), encoding="utf-8", newline="\n")
    print(f"sample font: {len(data)} bytes, {len(FONT_5X7)} glyphs")


def main():
    (ROOT / "lib/font/src").mkdir(parents=True, exist_ok=True)
    write_fixtures()
    write_sample()
    return 0


if __name__ == "__main__":
    sys.exit(main())
