#!/usr/bin/env python3
"""Writes a placeholder icon: a ringed planet on a dark rounded square, drawn here so that no image
from anywhere else is used. The committed SFCOP.icns is the application icon from
packaging/icon/ instead; this placeholder remains the Linux fallback when an .icns has no 512 px
entry. Running it with no argument overwrites SFCOP.icns.

    python3 packaging/macos/make_icon.py [OUT.icns]
"""

import math
import os
import struct
import sys
import zlib

SIZE = 512


def pixel(x, y):
    cx = cy = SIZE / 2
    # The rounded square, with a soft edge.
    margin, radius = 24, 96
    qx = max(abs(x + 0.5 - cx) - (cx - margin - radius), 0)
    qy = max(abs(y + 0.5 - cy) - (cy - margin - radius), 0)
    edge = radius - math.hypot(qx, qy)
    if edge <= -1:
        return (0, 0, 0, 0)
    alpha = int(255 * min(1.0, edge + 1))
    shade = y / SIZE
    r, g, b = int(10 + 14 * shade), int(14 + 20 * shade), int(40 + 40 * shade)
    # A few fixed stars.
    for sx, sy in ((90, 110), (400, 80), (430, 400), (120, 420), (300, 60), (70, 280)):
        if math.hypot(x - sx, y - sy) < 3:
            r = g = b = 230
    # The ring behind and in front of the planet: an ellipse band tilted a little.
    dx, dy = x - cx, y - cy
    tilt = math.radians(-18)
    rx = dx * math.cos(tilt) - dy * math.sin(tilt)
    ry = dx * math.sin(tilt) + dy * math.cos(tilt)
    ring = math.hypot(rx / 200, ry / 58)
    planet = math.hypot(dx, dy) < 120
    in_ring = 0.82 < ring < 1.0
    if planet and not (in_ring and ry > 0):
        light = max(0.25, 1 - math.hypot(dx + 50, dy + 50) / 230)
        r, g, b = int(220 * light), int(150 * light), int(60 * light)
    elif in_ring:
        r, g, b = 200, 190, 160
    return (r, g, b, alpha)


def png():
    rows = bytearray()
    for y in range(SIZE):
        rows.append(0)
        for x in range(SIZE):
            rows.extend(pixel(x, y))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(__file__), "SFCOP.icns")
    image = png()
    entry = b"ic09" + struct.pack(">I", 8 + len(image)) + image  # 512x512 PNG
    with open(out, "wb") as stream:
        stream.write(b"icns" + struct.pack(">I", 8 + len(entry)) + entry)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
