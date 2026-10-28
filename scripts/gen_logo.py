#!/usr/bin/env python3
"""Draws the TheoryCraftOS logo (assets/logo.png): an isometric cube whose three faces are cyan, magenta and orange gradients, a "T" cut out of
the top face and circuit traces on the other two. It is drawn here, with the standard library only, so that it is crisp, reproducible and can be
changed. It is deliberately not mirror-symmetric (the T, the trace layout, the marks in the corners of the tile), so a texture that is flipped,
rotated or mirrored by the sampler is obvious when it is shown on the 3D test's cube.

Usage: scripts/gen_logo.py [output.png]      (default assets/logo.png, 256x256)
"""
import math
import struct
import sys
import zlib

SIZE = 256
SS = 3  # samples per pixel side
NAVY = (12, 14, 32)


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def inside(poly, x, y):
    n = len(poly)
    c = False
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def shrink(poly, k):
    cx = sum(p[0] for p in poly) / len(poly)
    cy = sum(p[1] for p in poly) / len(poly)
    return [(cx + (x - cx) * k, cy + (y - cy) * k) for x, y in poly]


def rounded_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


TOP, UR, LR, BOT, LL, UL, C = (128, 30), (213, 79), (213, 177), (128, 226), (43, 177), (43, 79), (128, 128)
FACES = {
    "top": (shrink([TOP, UR, C, UL], 0.95), (70, 235, 255), (150, 120, 255)),
    "left": (shrink([UL, C, BOT, LL], 0.95), (255, 70, 190), (110, 40, 190)),
    "right": (shrink([C, UR, LR, BOT], 0.95), (255, 190, 60), (255, 80, 60)),
}
# Gradient directions: from the first colour at the top of the face's bounding box to the second at its bottom.


def face_color(name, x, y):
    poly, c0, c1 = FACES[name]
    ys = [p[1] for p in poly]
    t = (y - min(ys)) / (max(ys) - min(ys))
    return lerp(c0, c1, min(max(t, 0.0), 1.0))


# The "T" in the top face's own coordinates: u runs from UL towards TOP, v from UL towards C.
def top_uv(x, y):
    ax, ay = TOP[0] - UL[0], TOP[1] - UL[1]
    bx, by = C[0] - UL[0], C[1] - UL[1]
    det = ax * by - ay * bx
    px, py = x - UL[0], y - UL[1]
    return (px * by - py * bx) / det, (ax * py - ay * px) / det


def in_t(u, v):
    bar = 0.18 <= u <= 0.82 and 0.20 <= v <= 0.38
    stem = 0.41 <= u <= 0.59 and 0.20 <= v <= 0.82
    return bar or stem


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


# Circuit traces: polylines (with a pad at the end) over the left and right faces, all different so the picture has no symmetry.
TRACES = [
    ("left", [(58, 120), (74, 120), (88, 134), (88, 160)]),
    ("left", [(62, 150), (70, 150), (82, 162), (100, 162), (110, 172)]),
    ("left", [(100, 150), (100, 190)]),
    ("right", [(198, 112), (176, 112), (164, 124), (164, 150)]),
    ("right", [(196, 150), (184, 150), (176, 158), (176, 178), (150, 192)]),
]


def trace_hit(x, y):
    for _, pts in TRACES:
        for i in range(len(pts) - 1):
            if seg_dist(x, y, *pts[i], *pts[i + 1]) <= 1.6:
                return True
        if math.hypot(x - pts[-1][0], y - pts[-1][1]) <= 4.2:
            return True
    return False


# A bar in the tile's top-right corner and a dot in the bottom-left: they say which way up and which way round the picture is.
def corner_marks(x, y):
    return (226 <= x <= 238 and 18 <= y <= 24) or math.hypot(x - 18, y - 238) <= 5


def sample(x, y):
    if not rounded_rect(x, y, 2, 2, SIZE - 2, SIZE - 2, 28):
        return None
    # The background: a vignette from a faintly blue middle to the navy edge.
    d = math.hypot(x - 128, y - 128) / 180
    col = lerp((26, 30, 66), NAVY, min(d, 1.0))
    for name in ("top", "left", "right"):
        if inside(FACES[name][0], x, y):
            col = face_color(name, x, y)
            if name == "top":
                u, v = top_uv(x, y)
                if in_t(u, v):
                    col = NAVY
            elif trace_hit(x, y):
                col = lerp(col, (255, 255, 255), 0.75)
            break
    # The rim of the tile.
    if not rounded_rect(x, y, 5, 5, SIZE - 5, SIZE - 5, 25):
        col = lerp(col, (90, 110, 200), 0.6)
    if corner_marks(x, y):
        col = (255, 255, 255)
    return col


def render():
    rows = []
    for py in range(SIZE):
        row = bytearray([0])  # PNG filter: none
        for px in range(SIZE):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    c = sample(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS)
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; a += 1
            n = SS * SS
            if a:
                row += bytes((round(r / a), round(g / a), round(b / a), round(255 * a / n)))
            else:
                row += bytes((0, 0, 0, 0))
        rows.append(bytes(row))
    return b"".join(rows)


def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/logo.png"
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(render(), 9)) + chunk(b"IEND", b"")
    with open(out, "wb") as f:
        f.write(png)
    print(out, len(png), "bytes")


main()
