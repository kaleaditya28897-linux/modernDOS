#!/usr/bin/env python3
"""Take QEMU screendumps when tests/compat/vga.c says its picture is up.

Usage: vgawatch.py debug.log monitor.sock prefix   ->  prefix16.ppm, prefix32.ppm"""
import socket
import sys
import time


def main():
    log, sock, prefix = sys.argv[1:4]
    done = set()
    end = time.time() + 400
    while time.time() < end and len(done) < 2:
        try:
            text = open(log, "rb").read()
        except OSError:
            text = b""
        for bits in ("16", "32"):
            if bits not in done and ("VGA: showing %s" % bits).encode() in text:
                time.sleep(1)           # the monitor draws about 30 times a second
                try:
                    s = socket.socket(socket.AF_UNIX)
                    s.connect(sock)
                    s.sendall(("screendump %s%s.ppm\n" % (prefix, bits)).encode())
                    time.sleep(0.8)
                    s.close()
                except OSError:
                    pass
                done.add(bits)
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
