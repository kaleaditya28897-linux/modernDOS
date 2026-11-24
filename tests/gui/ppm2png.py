#!/usr/bin/env python3
"""Convert QEMU screendumps (PPM) to PNG, next to the originals.
Usage: ppm2png.py shot.ppm ..."""
import struct
import sys
import zlib


def conv(src, dst):
    d = open(src, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))

    open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


for a in sys.argv[1:]:
    conv(a, a[:-4] + ".png")
