# -*- coding: utf-8 -*-
"""Writes the R World Radio icon into src/app.rdef.

HVIF is usually produced by Icon-O-Matic, but a flat-shaded design needs only
solid colour styles and closed straight-edged paths, and those are short
enough to emit directly. Coordinates live on the 0..64 grid HVIF uses; every
value in that range fits the one-byte form (byte = value + 32), so nothing
here emits the two-byte encoding.

The subject is an object rather than a page, so it is drawn as a full
isometric box -- top, left and right faces meeting at the near corner -- with
the dial and knobs on one flank and the speaker grille on the other. That is
the same axis the other R apps sit on.

Run from the project root:  python3 tools/make_icon.py
It rewrites the `resource vector_icon` block in src/app.rdef in place.
"""

import io
import math
import os
import re


# #pragma mark - HVIF

def _coord(v):
    b = int(round(v)) + 32
    if not 0 <= b <= 127:
        raise ValueError("coordinate %r outside the one-byte range" % v)
    return bytes([b])


def build_hvif(styles, paths, shapes):
    """styles: list of (r,g,b,a). paths: list of [(x,y), ...] polygons.
    shapes: list of (style_index, [path indices]) drawn in order."""
    out = bytearray(b"ncif")

    out.append(len(styles))
    for r, g, b, a in styles:
        out.append(0x01)                      # solid colour, RGBA
        out += bytes([r, g, b, a])

    out.append(len(paths))
    for points in paths:
        out.append(0x02 | 0x08)               # closed, no curves
        out.append(len(points))
        for x, y in points:
            out += _coord(x) + _coord(y)

    out.append(len(shapes))
    for style, path_indices in shapes:
        out.append(0x0A)                      # path source
        out.append(style)
        out.append(len(path_indices))
        for i in path_indices:
            out.append(i)
        out.append(0x00)                      # no transform, no flags

    return bytes(out)


def rdef(data):
    """The `vector_icon` alias rc accepts for resource(101, "BEOS:ICON")."""
    lines = ["resource vector_icon {"]
    hexstr = "".join("%02X" % b for b in data)
    for i in range(0, len(hexstr), 64):
        lines.append('\t$"%s"' % hexstr[i:i + 64])
    lines.append("};")
    return "\n".join(lines) + "\n"


# #pragma mark - the box

# The top face. O is the far corner; U runs to the right corner and V to the
# left one. Values stay inside 0..64, which is what keeps every HVIF
# coordinate to its one-byte form.
O = (32.0, 11.0)
U = (26.0, 13.0)
V = (-26.0, 13.0)

# The body, straight down: in this projection verticals stay vertical.
DEPTH = (0.0, 21.0)


def top(u, v):
    """(0,0) far corner, (1,0) right, (0,1) left, (1,1) near."""
    return (O[0] + u * U[0] + v * V[0], O[1] + u * U[1] + v * V[1])


def left(u, h):
    """The left flank. u runs from the left corner to the near one, h down."""
    base = top(0, 1)
    return (base[0] + u * U[0] + h * DEPTH[0],
            base[1] + u * U[1] + h * DEPTH[1])


def right(v, h):
    """The right flank. v runs from the right corner to the near one."""
    base = top(1, 0)
    return (base[0] + v * V[0] + h * DEPTH[0],
            base[1] + v * V[1] + h * DEPTH[1])


def face(projection, points):
    return [projection(a, b) for a, b in points]


def rect(projection, a0, b0, a1, b1):
    return face(projection, [(a0, b0), (a1, b0), (a1, b1), (a0, b1)])


def disc(projection, ca, cb, ra, rb, steps=14):
    return face(projection,
                [(ca + ra * math.cos(2 * math.pi * i / steps),
                  cb + rb * math.sin(2 * math.pi * i / steps))
                 for i in range(steps)])


def shade(color, factor):
    r, g, b, a = color
    return (max(0, min(255, int(r * factor))),
            max(0, min(255, int(g * factor))),
            max(0, min(255, int(b * factor))), a)


def build_box(body_color, behind, groups):
    """behind: (colour, [polygon, ...]) pairs drawn before the box, so the box
    covers where they join it -- an antenna root, say.
    groups: the same, drawn on top of the faces, in order."""
    # Light from the upper left: the top face is brightest, the left flank
    # takes a little shadow and the right one most of it.
    styles = [body_color, shade(body_color, 0.76), shade(body_color, 0.52)]
    colors = {}

    def style_for(color):
        if color not in colors:
            colors[color] = len(styles)
            styles.append(color)
        return colors[color]

    paths = []
    shapes = []

    def add(style, polys):
        indices = []
        for p in polys:
            indices.append(len(paths))
            paths.append(p)
        if indices:
            shapes.append((style, indices))

    for color, polys in behind:
        add(style_for(color), polys)

    add(1, [[top(0, 1), top(1, 1), left(1, 1), left(0, 1)]])
    add(2, [[top(1, 0), top(1, 1), right(1, 1), right(0, 1)]])
    add(0, [[top(0, 0), top(1, 0), top(1, 1), top(0, 1)]])

    for color, polys in groups:
        add(style_for(color), polys)

    data = build_hvif(styles, paths, shapes)
    return data, len(paths)


# #pragma mark - the radio

BODY = (214, 178, 122, 255)          # a warm case, the colour of the real thing
GRILLE = (58, 50, 42, 255)
SLAT = (150, 126, 92, 255)
DIAL = (244, 238, 220, 255)
NEEDLE = (198, 62, 46, 255)
KNOB = (72, 62, 52, 255)
METAL = (176, 182, 190, 255)


def antenna():
    """A rod from the far-left of the top face, leaning up and to the left.

    Drawn before the box so the case covers its root, which is what makes it
    read as coming out of the body rather than lying against it.
    """
    base = top(0.14, 0.20)
    tip = (6.0, 1.0)
    width = 1.6
    return [[(base[0] - width, base[1]), (base[0] + width, base[1] + 1.0),
             (tip[0] + width, tip[1] + 1.0), (tip[0] - width, tip[1])]]


def speaker():
    """The right flank: a grille panel with slats across it."""
    panel = [rect(right, 0.10, 0.16, 0.62, 0.86)]
    slats = [rect(right, 0.16, 0.16 + i * 0.14, 0.56, 0.22 + i * 0.14)
             for i in range(5)]
    return panel, slats


def controls():
    """The left flank: the tuning window above, two knobs below."""
    window = [rect(left, 0.10, 0.14, 0.90, 0.44)]
    needle = [rect(left, 0.60, 0.16, 0.66, 0.42)]
    knobs = [disc(left, 0.26, 0.68, 0.13, 0.16),
             disc(left, 0.62, 0.68, 0.13, 0.16)]
    return window, needle, knobs


BLOCK = re.compile(r"^resource vector_icon \{.*?^\};\n", re.S | re.M)


if __name__ == "__main__":
    panel, slats = speaker()
    window, needle, knobs = controls()
    handle = [rect(top, 0.22, 0.40, 0.78, 0.60)]

    data, count = build_box(
        BODY,
        [(METAL, antenna())],
        [(GRILLE, panel + knobs),
         (SLAT, slats + handle),
         (DIAL, window),
         (NEEDLE, needle)])

    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    path = os.path.join(root, "src", "app.rdef")
    text = io.open(path, encoding="utf-8").read()
    if len(BLOCK.findall(text)) != 1:
        raise SystemExit("src/app.rdef: expected exactly one vector_icon block")
    io.open(path, "w", encoding="utf-8").write(
        BLOCK.sub(lambda m: rdef(data), text, count=1))
    print("src/app.rdef: %d bytes, %d paths" % (len(data), count))
