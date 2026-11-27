#!/usr/bin/env python3
"""Play the start of the game for tests/compat/game.sh through the QEMU
monitor: Enter until it is in its graphics mode, then a picture every two
seconds for a while.

Usage: gameplay.py debug.log monitor.sock prefix   ->  prefix-NN.ppm"""
import os
import socket
import sys
import time


def mon(sock, cmd):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.4)
        s.close()
    except OSError:
        pass


def size(path):
    try:
        with open(path, "rb") as f:
            head = f.read(32).split(b"\n")
        w, h = head[1].split()
        return int(w), int(h)
    except (OSError, IndexError, ValueError):
        return 0, 0


def graphics(log, probe):
    """In a graphics mode? The monitor's VGA says so on a UEFI boot; the
    emulated card's picture is 640 wide in mode 13h and its relatives (720
    in text mode)."""
    try:
        if b"vga: mode 13h" in open(log, "rb").read():
            return True
    except OSError:
        pass
    return size(probe)[0] == 640


def main():
    log, sock, prefix = sys.argv[1:4]
    probe = prefix + "-probe.ppm"
    start = time.time()
    # the game's first screen (text) waits for a key
    time.sleep(20)
    while time.time() - start < 120:
        mon(sock, "screendump " + probe)
        if graphics(log, probe):
            break
        mon(sock, "sendkey ret")
        time.sleep(4)
    else:
        print("   the game did not reach its graphics mode")
        return 1
    if os.path.exists(probe):
        os.remove(probe)
    for n in range(24):                 # loading, title, main menu, demo
        time.sleep(2)
        mon(sock, "screendump %s-%02d.ppm" % (prefix, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
