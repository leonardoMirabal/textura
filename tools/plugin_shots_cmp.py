#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The plugin window's pictures from another system (tools/plugin_linux.sh, tools/plugin_win.sh test) against macOS's
(make plugin-shots): each byte-identical, or within 2 levels on at most 0.1 % of its pixels. The window's drawing is
the same C on every system, but the C library's sinf, powf, atan2f ... may differ in the last bit between systems, which
can move an edge pixel by one level.

    python3 -I tools/plugin_shots_cmp.py DIR MACDIR"""
import os
import struct
import sys
import zlib


def pixels(path):
    """(width, height, channels, raw rows): the gate's own PNGs (8 bits, filter 0 on every row), else None"""
    d = open(path, "rb").read()
    i, idat, w, h, ch = 8, b"", 0, 0, 0
    while i < len(d):
        n, = struct.unpack(">I", d[i:i + 4])
        t = d[i + 4:i + 8]
        if t == b"IHDR":
            w, h, depth, ct = struct.unpack(">IIBB", d[i + 8:i + 18])
            if depth != 8 or ct not in (0, 2, 4, 6):
                return None
            ch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
        elif t == b"IDAT":
            idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    s = w * ch + 1
    if len(raw) != s * h or any(raw[y * s] for y in range(h)):
        return None
    return w, h, ch, raw


def main():
    src, mac = sys.argv[1], sys.argv[2]
    names = sorted(f for f in os.listdir(src) if f.endswith(".png")) if os.path.isdir(src) else []
    if not names:   # (the gate ran without its gui part: nothing to compare)
        print("skip  window pictures against macOS: none in %s" % src)
        return 0
    same = near = 0
    for f in names:
        a, b = os.path.join(src, f), os.path.join(mac, f)
        if not os.path.exists(b):
            print("FAIL  %s: macOS has no such picture" % f)
            continue
        if open(a, "rb").read() == open(b, "rb").read():
            same += 1
            continue
        pa, pb = pixels(a), pixels(b)
        if not pa or not pb or pa[:3] != pb[:3]:
            print("FAIL  %s differs from macOS's (size or format)" % f)
            continue
        w, h, ch, ra = pa
        rb, s = pb[3], w * ch + 1
        px, top = set(), 0
        for i in range(len(ra)):
            if ra[i] != rb[i]:
                top = max(top, abs(ra[i] - rb[i]))
                y, x = divmod(i, s)
                px.add((y, (x - 1) // ch))
        if top <= 2 and len(px) <= w * h // 1000:
            near += 1
            print("info  %s: %d pixels differ from macOS's, by at most %d levels" % (f, len(px), top))
        else:
            print("FAIL  %s: %d pixels differ from macOS's, by up to %d levels" % (f, len(px), top))
    n = len(names)
    if n and same + near == n:
        print("ok    window pictures: %d of %d byte-identical to macOS's%s" % (
            same, n, ", the others within 2 levels" if near else ""))
        return 0
    print("FAIL  window pictures: %d of %d byte-identical to macOS's, %d within 2 levels" % (same, n, near))
    return 1


if __name__ == "__main__":
    sys.exit(main())
