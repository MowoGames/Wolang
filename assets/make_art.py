#!/usr/bin/env python3
"""The project's logo and mascot, as data rather than as a file.

Both are drawn from the same shapes here, so the SVG a page embeds and
the PNG an extension ships cannot drift apart. No dependencies: the
rasteriser is a scanline fill with four-times supersampling and the PNG
writer is zlib and struct, because a build that needs a drawing library
to make an icon is a build nobody can run (D174).

    python3 assets/make_art.py
"""

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The palette. Night for the ground, ice for the fur, indigo for the
# shadow inside it, amber for what is awake.
NIGHT = (14, 18, 34)
DEEP = (24, 30, 56)
ICE = (226, 233, 247)
SHADE = (154, 167, 196)
INDIGO = (76, 91, 212)
AMBER = (255, 176, 32)
EMBER = (242, 110, 48)


def wolf_head(scale=1.0, dx=0.0, dy=0.0):
    """The mark: a wolf's head that is also a W.

    The two ears are the outer peaks and the chin is the middle one, so
    the silhouette reads as the letter at a glance and as the animal
    when you look.
    """
    def p(points):
        return [(x * scale + dx, y * scale + dy) for x, y in points]

    head = p([
        (256, 430),   # chin
        (150, 330),   # left jaw
        (112, 182),   # left cheek
        (92, 86),     # left ear
        (168, 152),   # left ear base
        (256, 126),   # crown
        (344, 152),   # right ear base
        (420, 86),    # right ear
        (400, 182),   # right cheek
        (362, 330),   # right jaw
    ])
    left_ear = p([(92, 86), (168, 152), (136, 176)])
    right_ear = p([(420, 86), (344, 152), (376, 176)])
    # Slanted, with the inner corner lower than the outer: the one
    # line that makes a shape read as a wolf rather than as a cat.
    left_eye = p([(162, 198), (232, 222), (230, 258), (166, 234)])
    right_eye = p([(350, 198), (280, 222), (282, 258), (346, 234)])
    muzzle = p([(256, 424), (210, 312), (302, 312)])
    nose = p([(256, 392), (234, 344), (278, 344)])
    return head, left_ear, right_ear, left_eye, right_eye, muzzle, nose


def mascot():
    """The mascot: the same wolf, sitting, in low polygons.

    Low-poly because the drawing is flat shapes, and a flat shape drawn
    honestly looks better than a curve faked with twelve of them. Seated
    rather than standing: a mascot that sits fits a square, which is the
    shape every place that will show it wants.
    """
    # A tail that starts at the hip and sweeps out and up, tapering.
    # The tail starts inside the haunches, so it belongs to the animal
    # rather than floating beside it.
    tail = [(196, 452), (130, 456), (80, 420), (68, 348), (104, 334),
            (110, 396), (150, 422), (198, 424)]
    # The haunches: wide at the ground, narrowing to the shoulders.
    body = [(152, 462), (160, 392), (188, 330), (240, 302), (272, 302),
            (324, 330), (352, 392), (360, 462)]
    # The front legs, with paws.
    left_leg = [(206, 462), (210, 384), (240, 384), (238, 462)]
    right_leg = [(274, 462), (272, 384), (302, 384), (306, 462)]
    left_paw = [(198, 462), (202, 438), (246, 438), (246, 462)]
    right_paw = [(266, 462), (266, 438), (310, 438), (314, 462)]
    # A tuft of chest fur, narrow enough that the legs stay legs.
    chest = [(256, 312), (238, 400), (274, 400)]
    head = [(256, 322), (182, 258), (164, 160), (148, 88), (216, 140),
            (256, 122), (296, 140), (364, 88), (348, 160), (330, 258)]
    left_ear = [(148, 88), (216, 140), (186, 166)]
    right_ear = [(364, 88), (296, 140), (326, 166)]
    left_eye = [(190, 180), (238, 194), (236, 218), (192, 206)]
    right_eye = [(322, 180), (274, 194), (276, 218), (320, 206)]
    snout = [(256, 318), (222, 240), (290, 240)]
    nose = [(256, 296), (240, 264), (272, 264)]
    return (tail, body, chest, left_leg, right_leg, head, left_ear,
            right_ear, left_eye, right_eye, snout, nose, left_paw,
            right_paw)


# ------------------------------------------------------------- raster ---


class Canvas:
    """A buffer of pixels, and polygons filled into it.

    Four samples across and four down per pixel: enough that a diagonal
    edge looks like an edge rather than a staircase, and cheap enough
    that the whole thing runs in a second.
    """

    SAMPLES = 4

    def __init__(self, size, background):
        self.size = size
        self.big = size * self.SAMPLES
        start = list(background)
        if len(start) == 3:
            start = start + [255]
        self.pixels = [list(start) for _ in range(self.big * self.big)]

    def fill(self, colour):
        for i in range(len(self.pixels)):
            self.pixels[i] = list(colour) + [255]

    def polygon(self, points, colour):
        pts = [(x * self.SAMPLES, y * self.SAMPLES) for x, y in points]
        top = max(0, int(min(y for _, y in pts)))
        bottom = min(self.big - 1, int(max(y for _, y in pts)) + 1)
        for y in range(top, bottom + 1):
            centre = y + 0.5
            crossings = []
            for i in range(len(pts)):
                x0, y0 = pts[i]
                x1, y1 = pts[(i + 1) % len(pts)]
                if y0 == y1:
                    continue
                if (y0 <= centre < y1) or (y1 <= centre < y0):
                    t = (centre - y0) / (y1 - y0)
                    crossings.append(x0 + t * (x1 - x0))
            crossings.sort()
            for i in range(0, len(crossings) - 1, 2):
                left = max(0, int(crossings[i] + 0.5))
                right = min(self.big - 1, int(crossings[i + 1] - 0.5))
                row = y * self.big
                for x in range(left, right + 1):
                    self.pixels[row + x] = list(colour) + [255]

    def rounded_rect(self, x0, y0, x1, y1, radius, colour):
        # A rectangle with the corners cut by a quarter circle, as a
        # polygon: the rasteriser only knows polygons, and sixteen
        # segments a corner is past what the eye can tell.
        points = []
        steps = 16
        import math
        for cx, cy, start in ((x1 - radius, y0 + radius, -90),
                              (x1 - radius, y1 - radius, 0),
                              (x0 + radius, y1 - radius, 90),
                              (x0 + radius, y0 + radius, 180)):
            for s in range(steps + 1):
                angle = math.radians(start + 90 * s / steps)
                points.append((cx + radius * math.cos(angle),
                               cy + radius * math.sin(angle)))
        self.polygon(points, colour)

    def write(self, path):
        size = self.size
        rows = []
        step = self.SAMPLES
        for y in range(size):
            row = bytearray()
            row.append(0)
            for x in range(size):
                r = g = b = a = 0
                for sy in range(step):
                    base = (y * step + sy) * self.big + x * step
                    for sx in range(step):
                        pixel = self.pixels[base + sx]
                        r += pixel[0]
                        g += pixel[1]
                        b += pixel[2]
                        a += pixel[3]
                count = step * step
                row += bytes((r // count, g // count, b // count,
                              a // count))
            rows.append(bytes(row))
        raw = b"".join(rows)

        def chunk(kind, payload):
            return (struct.pack(">I", len(payload)) + kind + payload +
                    struct.pack(">I", zlib.crc32(kind + payload) &
                                0xFFFFFFFF))

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR",
                     struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(raw, 9))
        png += chunk(b"IEND", b"")
        path.write_bytes(png)


# ---------------------------------------------------------------- svg ---


def svg_polygon(points, colour):
    body = " ".join("{:.1f},{:.1f}".format(x, y) for x, y in points)
    return '  <polygon points="{}" fill="rgb{}"/>\n'.format(body, colour)


def write_logo_svg(path, with_ground=True):
    head, left_ear, right_ear, left_eye, right_eye, muzzle, nose = \
        wolf_head()
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512"'
           ' width="512" height="512">\n']
    if with_ground:
        out.append('  <rect width="512" height="512" rx="112" '
                   'fill="rgb{}"/>\n'.format(NIGHT))
    out.append(svg_polygon(head, ICE))
    out.append(svg_polygon(left_ear, INDIGO))
    out.append(svg_polygon(right_ear, INDIGO))
    out.append(svg_polygon(muzzle, SHADE))
    out.append(svg_polygon(nose, DEEP))
    out.append(svg_polygon(left_eye, AMBER))
    out.append(svg_polygon(right_eye, AMBER))
    out.append('</svg>\n')
    path.write_text("".join(out), encoding="utf-8")


def write_mascot_svg(path):
    shapes = mascot()
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512"'
           ' width="512" height="512">\n',
           '  <rect width="512" height="512" rx="112" fill="rgb{}"/>\n'
           .format(NIGHT)]
    for index, colour in MASCOT_ORDER:
        out.append(svg_polygon(shapes[index], colour))
    out.append('</svg>\n')
    path.write_text("".join(out), encoding="utf-8")


def write_wordmark(path):
    """The mark beside the name, for a page header."""
    head, left_ear, right_ear, left_eye, right_eye, muzzle, nose = \
        wolf_head(scale=0.42, dx=24, dy=22)
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 760 240"'
           ' width="760" height="240">\n',
           '  <rect width="760" height="240" fill="none"/>\n']
    for shape, colour in ((head, ICE), (left_ear, INDIGO),
                          (right_ear, INDIGO), (muzzle, SHADE),
                          (nose, DEEP), (left_eye, AMBER),
                          (right_eye, AMBER)):
        out.append(svg_polygon(shape, colour))
    out.append('  <text x="268" y="152" font-family="Inter, Helvetica, '
               'Arial, sans-serif" font-size="108" font-weight="700" '
               'fill="rgb{}">Wolang</text>\n'.format(ICE))
    out.append('  <text x="272" y="196" font-family="Inter, Helvetica, '
               'Arial, sans-serif" font-size="26" letter-spacing="6" '
               'fill="rgb{}">COMPILED. TYPED. FAST.</text>\n'
               .format(AMBER))
    out.append('</svg>\n')
    path.write_text("".join(out), encoding="utf-8")


def render_logo(size, path, rounded=True):
    # Transparent outside the rounded square: an icon with square
    # corners drawn under a rounded mask is an icon with a dark halo.
    canvas = Canvas(size, (0, 0, 0, 0) if rounded else NIGHT)
    unit = size / 512.0
    if rounded:
        canvas.rounded_rect(0, 0, size, size, 0.22 * size, NIGHT)
    head, left_ear, right_ear, left_eye, right_eye, muzzle, nose = \
        wolf_head(scale=unit)
    for shape, colour in ((head, ICE), (left_ear, INDIGO),
                          (right_ear, INDIGO), (muzzle, SHADE),
                          (nose, DEEP), (left_eye, AMBER),
                          (right_eye, AMBER)):
        canvas.polygon(shape, colour)
    canvas.write(path)


# What is drawn, in the order it is drawn: the tail behind everything,
# the eyes last.
MASCOT_ORDER = (
    (0, SHADE),   # tail
    (1, ICE),     # haunches
    (3, ICE),     # left leg
    (4, ICE),     # right leg
    (12, SHADE),  # left paw
    (13, SHADE),  # right paw
    (2, SHADE),   # chest
    (5, ICE),     # head
    (6, INDIGO),  # left ear
    (7, INDIGO),  # right ear
    (10, SHADE),  # snout
    (11, DEEP),   # nose
    (8, AMBER),   # left eye
    (9, AMBER),   # right eye
)


def render_mascot(size, path):
    canvas = Canvas(size, (0, 0, 0, 0))
    unit = size / 512.0
    canvas.rounded_rect(0, 0, size, size, 0.22 * size, NIGHT)
    shapes = mascot()
    for index, colour in MASCOT_ORDER:
        points = [(x * unit, y * unit) for x, y in shapes[index]]
        canvas.polygon(points, colour)
    canvas.write(path)


def write_ico(path, sizes=(16, 32, 48, 64, 128, 256)):
    """The mark as a Windows icon.

    An `.ico` is a small header and then a PNG per size, which is what
    the format has allowed since Vista -- so the PNGs this already
    writes are the whole of it (D188).
    """
    images = []
    for size in sizes:
        canvas = Canvas(size, (0, 0, 0, 0))
        unit = size / 512.0
        canvas.rounded_rect(0, 0, size, size, 0.22 * size, NIGHT)
        head, left_ear, right_ear, left_eye, right_eye, muzzle, nose = \
            wolf_head(scale=unit)
        for shape, colour in ((head, ICE), (left_ear, INDIGO),
                              (right_ear, INDIGO), (muzzle, SHADE),
                              (nose, DEEP), (left_eye, AMBER),
                              (right_eye, AMBER)):
            canvas.polygon(shape, colour)
        held = HERE / ".ico-{}.png".format(size)
        canvas.write(held)
        images.append((size, held.read_bytes()))
        held.unlink()

    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    body = b""
    for size, data in images:
        entries += struct.pack("<BBBBHHII",
                               0 if size >= 256 else size,
                               0 if size >= 256 else size,
                               0, 0, 1, 32, len(data), offset)
        body += data
        offset += len(data)
    path.write_bytes(header + entries + body)


def main():
    write_logo_svg(HERE / "logo.svg")
    write_mascot_svg(HERE / "mascot.svg")
    write_wordmark(HERE / "wordmark.svg")
    for size in (512, 256, 128, 64):
        render_logo(size, HERE / "logo-{}.png".format(size))
    render_mascot(512, HERE / "mascot-512.png")
    render_mascot(256, HERE / "mascot-256.png")
    # What the editor extension ships.
    render_logo(128, HERE.parent / "editors" / "vscode" / "icon.png")
    # What the Windows installer shows (D188).
    write_ico(HERE / "wolang.ico")
    print("wrote the logo, the wordmark and the mascot")


if __name__ == "__main__":
    main()
