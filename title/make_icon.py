#!/usr/bin/env python3
"""Generates title/icon0.png (512x512 RGB PNG) using only the Python standard library.

    make_icon.py OUT.png
"""
import math
import struct
import sys
import zlib


def point_seg_dist(px: float, py: float, ax: float, ay: float, bx: float, by: float) -> float:
    dx, dy = bx - ax, by - ay
    denom = dx * dx + dy * dy
    if denom == 0:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / denom))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def in_round_rect(x: float, y: float, x0: float, y0: float, x1: float, y1: float, r: float) -> bool:
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def generate(out_path: str) -> None:
    size = 512
    rows = []
    for y in range(size):
        py = y + 0.5
        t = py / size
        bg_r = int(14 + (8 - 14) * t)
        bg_g = int(18 + (11 - 18) * t)
        bg_b = int(32 + (20 - 32) * t)
        row = bytearray(1 + size * 3)
        row[0] = 0
        for x in range(size):
            px = x + 0.5
            r, g, b = bg_r, bg_g, bg_b
            if in_round_rect(px, py, 76, 108, 436, 404, 68):
                r, g, b = 220, 32, 28
                # White upload arrow + tray inside YouTube badge
                white = (
                    point_seg_dist(px, py, 256, 178, 256, 306) <= 18
                    or point_seg_dist(px, py, 256, 172, 198, 232) <= 18
                    or point_seg_dist(px, py, 256, 172, 314, 232) <= 18
                    or point_seg_dist(px, py, 168, 342, 344, 342) <= 16
                    or point_seg_dist(px, py, 168, 342, 168, 312) <= 16
                    or point_seg_dist(px, py, 344, 342, 344, 312) <= 16
                )
                if white:
                    r, g, b = 255, 255, 255
            o = 1 + x * 3
            row[o], row[o + 1], row[o + 2] = r, g, b
        rows.append(bytes(row))

    raw = b"".join(rows)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    with open(out_path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: make_icon.py OUT.png", file=sys.stderr)
        sys.exit(2)
    generate(sys.argv[1])
