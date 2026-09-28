#!/usr/bin/env python3
"""Generates the Ksila launcher icon (android/res/mipmap-xxhdpi/ic_launcher.png).

Draws a Godot-style dark rounded square with a #478cbf button and a white
play triangle — geometric, no font rendering needed. Pure-Python PNG encoder
(zlib is in the stdlib).
"""

import math
import os
import struct
import zlib

SIZE = 144  # xxhdpi launcher (48dp * 3)


def rounded_square_sdf(x, y, cx, cy, half, radius):
    qx = abs(x - cx) - (half - radius)
    qy = abs(y - cy) - (half - radius)
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    inside = min(max(qx, qy), 0.0)
    return outside + inside - radius


def triangle_sdf(x, y, p0, p1, p2):
    # Signed distance to a triangle (negative inside).
    def edge(px, py, ax, ay, bx, by):
        ex, ey = bx - ax, by - ay
        wx, wy = px - ax, py - ay
        t = max(0.0, min(1.0, (wx * ex + wy * ey) / (ex * ex + ey * ey)))
        dx, dy = wx - t * ex, wy - t * ey
        return dx * dx + dy * dy
    d = min(edge(x, y, *p0, *p1), edge(x, y, *p1, *p2), edge(x, y, *p2, *p0))
    # Winding for the inside test.
    sign = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])
    px = (x - p0[0], y - p0[1])
    cross1 = (p1[0] - p0[0]) * px[1] - (p1[1] - p0[1]) * px[0]
    cross2 = (p2[0] - p1[0]) * (y - p1[1]) - (p2[1] - p1[1]) * (x - p1[0])
    cross3 = (p0[0] - p2[0]) * (y - p2[1]) - (p0[1] - p2[1]) * (x - p2[0])
    inside = (cross1 >= 0 and cross2 >= 0 and cross3 >= 0) or (cross1 <= 0 and cross2 <= 0 and cross3 <= 0)
    if inside:
        return -math.sqrt(d)
    return math.sqrt(d)


def coverage(sdf, aa=1.25):
    if sdf <= -aa:
        return 1.0
    if sdf >= aa:
        return 0.0
    return (aa - sdf) / (2.0 * aa)


def main():
    c = SIZE / 2.0
    # Colors.
    bg = (35, 39, 47)  # #23272f, фон лобби
    blue = (71, 140, 191)  # #478cbf, Godot Blue
    white = (235, 235, 235)

    pixels = []
    for y in range(SIZE):
        for x in range(SIZE):
            px = x + 0.5
            py = y + 0.5
            # Background rounded square (full icon with rounded corners).
            a_bg = coverage(rounded_square_sdf(px, py, c, c, c - 6, 26))
            # Blue button.
            a_btn = coverage(rounded_square_sdf(px, py, c, c, 44, 20))
            # Play triangle.
            tri = ((c - 16, c - 22), (c - 16, c + 22), (c + 26, c))
            a_tri = coverage(triangle_sdf(px, py, *tri), aa=1.0)

            r, g, b = bg
            if a_btn > 0.0:
                r = blue[0] * a_btn + r * (1 - a_btn)
                g = blue[1] * a_btn + g * (1 - a_btn)
                b = blue[2] * a_btn + b * (1 - a_btn)
            if a_tri > 0.0:
                r = white[0] * a_tri + r * (1 - a_tri)
                g = white[1] * a_tri + g * (1 - a_tri)
                b = white[2] * a_tri + b * (1 - a_tri)
            pixels.append((int(round(r)), int(round(g)), int(round(b)), int(round(a_bg * 255))))

    # Encode PNG (RGBA, filter 0).
    raw = b"".join(
        b"\x00" + bytes(v for p in pixels[y * SIZE:(y + 1) * SIZE] for v in p)
        for y in range(SIZE)
    )

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

    out = os.path.join(os.path.dirname(__file__), "..", "android", "res", "mipmap-xxhdpi", "ic_launcher.png")
    out = os.path.normpath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(png)
    print(f"icon: wrote {out} ({SIZE}x{SIZE}, {len(png)} bytes)")


if __name__ == "__main__":
    main()
