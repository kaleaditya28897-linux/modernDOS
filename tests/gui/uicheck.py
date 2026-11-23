#!/usr/bin/env python3
"""Check UITEST's results (see uitest.c, uidrive.py).

Usage: uicheck.py debug.log screen.ppm
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxcheck import read_ppm  # noqa: E402

ACCENT = (32, 128, 136)
SELECTED_UNFOCUSED = (196, 204, 214)


def main():
    log = open(sys.argv[1], "rb").read().decode("latin-1")
    w = {m[0]: (int(m[1]), int(m[2]))
         for m in re.findall(r"UITEST: widget (\w+) (\d+),(\d+)", log)}
    checks = [
        ("OK: typed text, checkbox on (Tab+Space), Item 3",
         "UITEST: ok text=hello check=1 list=Item 3" in log),
        ("File/Quit chosen with the keyboard", "UITEST: menu File/Quit" in log),
        ("UITEST finished", "UITEST: done" in log),
    ]
    try:
        width, h, px = read_ppm(sys.argv[2])

        def pixel(x, y):
            i = (y * width + x) * 3
            return tuple(px[i:i + 3])

        cx, cy = w["checkbox"]
        checks.append(("checkbox drawn checked", pixel(cx, cy) == ACCENT))
        ix, iy = w["item3"]
        checks.append(("Item 3 drawn selected", pixel(ix + 60, iy - 7) == SELECTED_UNFOCUSED))
        tx, ty = w["text"]
        dark = sum(1 for x in range(tx - 106, tx - 60) for y in range(ty - 6, ty + 6)
                   if sum(pixel(x, y)) < 200)
        checks.append(("text field shows the typed text", dark > 20))
    except (OSError, ValueError, KeyError) as e:
        checks.append(("screenshot: %s" % e, False))
    fail = 0
    for name, ok in checks:
        print("  %s ui: %s" % ("ok  " if ok else "FAIL", name))
        fail |= not ok
    return fail


if __name__ == "__main__":
    sys.exit(main())
