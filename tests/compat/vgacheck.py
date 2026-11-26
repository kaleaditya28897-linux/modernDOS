#!/usr/bin/env python3
"""Check a QEMU screendump of tests/compat/vga.c: red above, green below.

Usage: vgacheck.py shot.ppm"""
import sys


def main():
    d = open(sys.argv[1], "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]

    def at(x, y):
        o = (y * w + x) * 3
        return tuple(px[o:o + 3])

    top = at(w // 2, h // 2 - h // 8)
    bottom = at(w // 2, h // 2 + h // 8)
    good = top[0] > 200 and top[1] < 60 and top[2] < 60 and bottom[1] > 200 and bottom[0] < 60 and bottom[2] < 60
    print("     %dx%d, above the middle %s, below %s" % (w, h, top, bottom))
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
