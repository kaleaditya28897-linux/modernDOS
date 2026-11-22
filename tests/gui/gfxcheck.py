#!/usr/bin/env python3
"""Check a screenshot (PPM) of GFXTEST's pattern.

Usage: gfxcheck.py screen.ppm debug.log
The mode size comes from the "GFXTEST: drawn WxH" line in the debug log.
Prints ok/FAIL lines, exit code 1 on failure.
"""
import re
import sys


def read_ppm(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:                      # P6, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    pos += 1
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError("not a P6 PPM with maxval 255")
    return int(fields[1]), int(fields[2]), data[pos:]


def main():
    ppm, log = sys.argv[1], sys.argv[2]
    m = re.search(rb"GFXTEST: drawn (\d+)x(\d+)", open(log, "rb").read())
    if not m:
        print("  FAIL gfx: no 'GFXTEST: drawn' in the log")
        return 1
    w, h = int(m.group(1)), int(m.group(2))
    try:
        pw, ph, px = read_ppm(ppm)
    except (OSError, ValueError) as e:
        print("  FAIL gfx: screenshot: %s" % e)
        return 1

    def pixel(x, y):
        i = (y * pw + x) * 3
        return tuple(px[i:i + 3])

    checks = [
        ("screenshot is %dx%d" % (w, h), (pw, ph) == (w, h)),
        ("top left red", pixel(60, 60) == (255, 0, 0)),
        ("top right green", pixel(w - 60, 60) == (0, 255, 0)),
        ("bottom left blue", pixel(60, h - 60) == (0, 0, 255)),
        ("bottom right white", pixel(w - 60, h - 60) == (255, 255, 255)),
        ("background", pixel(w // 2, h // 4) == (32, 48, 64)),
        ("clipped fill left the red box alone", pixel(122, 60) == (32, 48, 64)
         and pixel(5, 5) == (255, 0, 0)),
        ("gradient: top darker than bottom", sum(pixel(w // 2, h // 2 - 58)) <
         sum(pixel(w // 2, h // 2 - 23))),
    ]
    fail = 0
    for name, ok in checks:
        print("  %s gfx: %s" % ("ok  " if ok else "FAIL", name))
        fail |= not ok
    return fail


if __name__ == "__main__":
    sys.exit(main())
