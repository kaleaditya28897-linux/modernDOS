#!/usr/bin/env python3
"""Compare the pictures of tests/compat/game.sh: the screens of the game
that stand still (the same in two pictures in a row) on the BIOS edition
must turn up on the UEFI boot too, looking the same.

Pictures are brought to 320x200: the emulated VGA card shows the mode as
640x400, the monitor's VGA enlarges it by a whole factor in the middle of
the framebuffer.

Usage: gamecheck.py bios-prefix uefi-prefix"""
import glob
import sys

W, H = 320, 200


def load(path):
    d = open(path, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    s = min(w // W, h // H)
    if s < 1:
        return None
    x0, y0 = (w - W * s) // 2, (h - H * s) // 2
    out = bytearray(W * H * 3)
    for y in range(H):
        row = ((y0 + y * s + s // 2) * w + x0 + s // 2) * 3
        for x in range(W):
            o = row + x * s * 3
            out[(y * W + x) * 3:(y * W + x) * 3 + 3] = px[o:o + 3]
    return bytes(out)


def still(prefix):
    """The pictures that are the same as the one before them."""
    frames, last, found = sorted(glob.glob(prefix + "-[0-9][0-9].ppm")), None, []
    for f in frames:
        p = load(f)
        if p is not None and p == last and p not in found:
            found.append(p)
        last = p
    return found, len(frames)


def colours(p):
    return len({p[i:i + 3] for i in range(0, len(p), 3)})


def differ(a, b):
    """Share of pixels that differ by more than the rounding of a 6 bit
    colour component."""
    n = 0
    for i in range(0, len(a), 3):
        if abs(a[i] - b[i]) > 6 or abs(a[i + 1] - b[i + 1]) > 6 or abs(a[i + 2] - b[i + 2]) > 6:
            n += 1
    return n / (len(a) // 3)


def main():
    bios, nb = still(sys.argv[1])
    uefi, nu = still(sys.argv[2])
    print("     BIOS: %d pictures, %d still screens; UEFI: %d pictures, %d still screens" %
          (nb, len(bios), nu, len(uefi)))
    rich = [p for p in bios if colours(p) >= 24]
    if not rich:
        print("     no still screen with many colours on the BIOS edition")
        return 1
    matched = 0
    for p in rich:
        best = min((differ(p, q) for q in uefi), default=1.0)
        print("     a screen of %d colours: %.1f%% of its pixels differ on UEFI" % (colours(p), best * 100))
        if best < 0.02:
            matched += 1
    return 0 if matched else 1


if __name__ == "__main__":
    sys.exit(main())
