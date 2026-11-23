#!/usr/bin/env python3
"""Check MOUSETEST's results: the click position and the screenshot.

Usage: mousecheck.py debug.log screen.ppm [loose]
The click must be at the start position + (120, 60) (mousedrive.py); with
"loose" (a mouse driver with acceleration, e.g. CTMOUSE) only right and
down. The screenshot must show the orange square there and the cursor.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxcheck import read_ppm  # noqa: E402

DX, DY = 120, 60


def main():
    log = open(sys.argv[1], "rb").read().decode("latin-1")
    ready = re.search(r"MOUSETEST: ready (\d+)x(\d+) at (\d+),(\d+), mouse (\w+)", log)
    click = re.search(r"MOUSETEST: click at (\d+),(\d+)", log)
    checks = [("mouse driver found", bool(ready) and ready.group(5) == "yes"),
              ("a click arrived", bool(click))]
    if ready and click:
        x0, y0 = int(ready.group(3)), int(ready.group(4))
        cx, cy = int(click.group(1)), int(click.group(2))
        if len(sys.argv) > 3 and sys.argv[3] == "loose":
            checks.append(("click moved right and down: (%d,%d)" % (cx - x0, cy - y0),
                           cx > x0 and cy > y0))
        else:
            checks.append(("click at start + (%d,%d): got (%d,%d)" % (DX, DY, cx - x0, cy - y0),
                           (cx - x0, cy - y0) == (DX, DY)))
        try:
            w, h, px = read_ppm(sys.argv[2])

            def pixel(x, y):
                i = (y * w + x) * 3
                return tuple(px[i:i + 3])

            checks.append(("orange square at the click", pixel(cx - 8, cy - 8) == (255, 140, 0)))
            # the cursor's tip is at the click position: outline, then fill
            checks.append(("cursor drawn at the click",
                           pixel(cx, cy) == (16, 16, 16) and pixel(cx + 1, cy + 3) == (255, 255, 255)))
        except (OSError, ValueError) as e:
            checks.append(("screenshot: %s" % e, False))
    checks.append(("MOUSETEST finished", "MOUSETEST: done" in log))
    fail = 0
    for name, ok in checks:
        print("  %s mouse: %s" % ("ok  " if ok else "FAIL", name))
        fail |= not ok
    return fail


if __name__ == "__main__":
    sys.exit(main())
