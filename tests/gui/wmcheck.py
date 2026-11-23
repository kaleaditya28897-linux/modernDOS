#!/usr/bin/env python3
"""Check WMTEST's results (see wmtest.c, wmdrive.py).

Usage: wmcheck.py debug.log screen.ppm
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxcheck import read_ppm  # noqa: E402

RED, GREEN, BLUE = (200, 60, 60), (60, 180, 90), (60, 100, 200)
TITLE_ACTIVE = (32, 128, 136)


def near(pos, want, tol=3):
    return pos is not None and abs(pos[0] - want[0]) <= tol and abs(pos[1] - want[1]) <= tol


def main():
    log = open(sys.argv[1], "rb").read().decode("latin-1")
    done = re.search(r"WMTEST: done(.*)", log)
    pos = {}
    if done:
        for name, x, y in re.findall(r"(\w+)=(\d+),(\d+)", done.group(1)):
            pos[name] = (int(x), int(y))
    checks = [
        ("Beta got the click", "WMTEST: click in Beta" in log),
        ("Alpha dragged to (300,350): %s" % (pos.get("Alpha"),), near(pos.get("Alpha"), (300, 350))),
        ("Beta and Gamma stayed", pos.get("Beta") == (300, 200) and pos.get("Gamma") == (600, 120)),
        ("Beta on top", bool(done) and "top=Beta" in done.group(1)),
    ]
    try:
        w, h, px = read_ppm(sys.argv[2])

        def pixel(x, y):
            i = (y * w + x) * 3
            return tuple(px[i:i + 3])

        checks += [
            ("overlap shows Beta (on top)", pixel(450, 380) == GREEN),
            ("moved Alpha below it", pixel(450, 500) == RED),
            ("Gamma unchanged", pixel(700, 250) == BLUE),
            ("Beta's title bar is active", pixel(400, 212) == TITLE_ACTIVE),
            ("Alpha's old place is desktop", pixel(150, 200) not in (RED, GREEN, BLUE)),
        ]
    except (OSError, ValueError) as e:
        checks.append(("screenshot: %s" % e, False))
    fail = 0
    for name, ok in checks:
        print("  %s wm: %s" % ("ok  " if ok else "FAIL", name))
        fail |= not ok
    return fail


if __name__ == "__main__":
    sys.exit(main())
