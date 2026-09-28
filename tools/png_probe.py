#!/usr/bin/env python3
"""Tiny dependency-free PNG reader + lobby layout probe.

Reads a PNG produced by ksila-preview and verifies the Godot-style lobby:
background gradient, button styleboxes (normal/hover/pressed), focus outline,
text presence, footer and the Godot-blue subtitle.

Usage: png_probe.py preview.png [--hover N --press N --focus N --status]
"""

import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    width = height = None
    bit_depth = color_type = None
    idat = b""
    palette = None
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"PLTE":
            palette = [tuple(chunk[i:i + 3]) for i in range(0, len(chunk), 3)]
        elif ctype == b"IEND":
            break
    assert bit_depth == 8, f"unsupported bit depth {bit_depth}"
    raw = zlib.decompress(idat)

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    stride = width * channels
    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        filter_type = raw[p]
        p += 1
        row = bytearray(raw[p:p + stride])
        p += stride
        if filter_type == 1:  # Sub
            for i in range(channels, stride):
                row[i] = (row[i] + row[i - channels]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                a = row[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        rows.append(bytes(row))
        prev = row

    def pixel(x, y):
        row = rows[y]
        if color_type == 3:
            return palette[row[x]]
        if channels == 1:
            v = row[x]
            return (v, v, v, 255)
        if channels == 2:
            v = row[x * 2]
            return (v, v, v, row[x * 2 + 1])
        base = x * channels
        if channels == 3:
            return (row[base], row[base + 1], row[base + 2], 255)
        return (row[base], row[base + 1], row[base + 2], row[base + 3])

    return width, height, pixel


def blend(bg, fg, alpha):
    return tuple(int(round(fg[i] * alpha + bg[i] * (1 - alpha))) for i in range(3))


def near(actual, expected, tol=4):
    return all(abs(a - e) <= tol for a, e in zip(actual, expected))


def main():
    path = sys.argv[1]
    args = sys.argv[2:]
    hover = args.index("--hover") if "--hover" in args else -1
    hover = int(args[hover + 1]) if hover >= 0 else -1
    press_i = args.index("--press") if "--press" in args else -1
    press = int(args[press_i + 1]) if press_i >= 0 else -1
    focus_i = args.index("--focus") if "--focus" in args else -1
    focus = int(args[focus_i + 1]) if focus_i >= 0 else -1
    has_status = "--status" in args

    w, h, px = read_png(path)
    print(f"image: {w}x{h}")

    failures = []

    def check(name, cond, info=""):
        status = "OK " if cond else "FAIL"
        print(f"  [{status}] {name} {info}")
        if not cond:
            failures.append(name)

    # ---- layout (must mirror src/ui.cpp) ----
    s = h / 720.0
    bw, bh, gap = 300 * s, 48 * s, 14 * s
    start_y = h * 0.42
    total = 3 * bh + 2 * gap
    if start_y + total > h - 70 * s:
        start_y = h - 70 * s - total
    button_x0 = w / 2 - bw / 2
    centers = [start_y + i * (bh + gap) + bh / 2 for i in range(3)]

    # ---- background gradient ----
    top_bg = (int(0.135 * 255 + 0.5), int(0.153 * 255 + 0.5), int(0.180 * 255 + 0.5))
    bottom_bg = (int(0.078 * 255 + 0.5), int(0.086 * 255 + 0.5), int(0.106 * 255 + 0.5))
    check("bg top", near(px(10, 4), top_bg), f"{px(10, 4)} ~ {top_bg}")
    check("bg bottom", near(px(10, h - 2), bottom_bg), f"{px(10, h - 2)} ~ {bottom_bg}")
    mid = blend(top_bg, bottom_bg, 0.5)
    check("bg mid", near(px(10, h // 2), mid), f"{px(10, h // 2)} ~ {mid}")

    # ---- button styleboxes (sample away from text, near the left edge) ----
    sample_x = int(button_x0 + 14 * s)
    for i in range(3):
        y = int(centers[i])
        bg_here = blend(top_bg, bottom_bg, y / h)
        state = "normal"
        fill = (26, 26, 26, 153)
        if press == i:
            state = "pressed"
            fill = (0, 0, 0, 153)
        elif hover == i:
            state = "hover"
            fill = (57, 57, 57, 153)
        expected = blend(bg_here, fill[:3], fill[3] / 255.0)
        # sampling right at the button row blends with rounded rect + msaa; use tol 6
        check(f"button {i} {state}", near(px(sample_x, y), expected, 6),
              f"{px(sample_x, y)} ~ {expected}")

    # ---- text present inside each button (bright pixels vs fill) ----
    for i in range(3):
        y0, y1 = int(centers[i] - 10 * s), int(centers[i] + 10 * s)
        x0, x1 = int(w / 2 - 80 * s), int(w / 2 + 80 * s)
        brightest = max(sum(px(x, yy)[:3]) / 3 for yy in range(y0, y1) for x in range(x0, x1, 2))
        check(f"button {i} text", brightest > 180, f"brightest {brightest:.0f}")

    # ---- title text present ----
    title_y = int(96 * s + 0.75 * 84 * s)
    brightest = max(sum(px(x, yy)[:3]) / 3
                    for yy in range(int(title_y - 40 * s), int(title_y + 10 * s), 2)
                    for x in range(int(w * 0.4), int(w * 0.6), 2))
    check("title text", brightest > 210, f"brightest {brightest:.0f}")

    # ---- subtitle: Godot blue #478cbf ----
    sub_y = int(title_y + 34 * s)
    found_blue = 0
    for yy in range(int(sub_y - 12 * s), int(sub_y + 12 * s)):
        for x in range(int(w * 0.4), int(w * 0.6)):
            r, g, b, _ = px(x, yy)
            if abs(r - 0x47) < 50 and abs(g - 0x8c) < 40 and abs(b - 0xbf) < 40:
                found_blue += 1
    check("subtitle godot-blue", found_blue > 30, f"blue pixels {found_blue}")

    # ---- focus outline (2px white-ish border just inside button edges) ----
    if focus >= 0:
        fy = int(centers[focus])
        x0 = int(button_x0)
        ring = px(x0 + 1, fy)  # 1px inside the left edge
        check(f"focus ring {focus}", ring[0] > 90 and ring[2] > 90, f"{ring}")
    else:
        # no white ring anywhere on button 0's left edge
        fy = int(centers[0])
        ring = px(int(button_x0) + 1, fy)
        check("no focus ring", ring[0] < 60, f"{ring}")

    # ---- status toast ----
    if has_status:
        ty = int(start_y + total + 44 * s)
        found = 0
        for yy in range(int(ty - 10 * s), int(ty + 10 * s)):
            for x in range(int(w * 0.3), int(w * 0.7), 2):
                if sum(px(x, yy)[:3]) / 3 > 200:
                    found += 1
        check("status toast", found > 20, f"bright px {found}")

    # ---- footer ----
    fy = int(h - 20 * s)
    found = 0
    for yy in range(int(fy - 12 * s), int(fy + 4 * s)):
        for x in range(int(20 * s), int(200 * s)):
            if sum(px(x, yy)[:3]) / 3 > 120:
                found += 1
    check("footer left", found > 20, f"px {found}")
    found = 0
    for yy in range(int(fy - 12 * s), int(fy + 4 * s)):
        for x in range(int(w - 260 * s), int(w - 20 * s)):
            if sum(px(x, yy)[:3]) / 3 > 120:
                found += 1
    check("footer right", found > 20, f"px {found}")

    print()
    if failures:
        print(f"FAILED: {len(failures)} checks: {failures}")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
