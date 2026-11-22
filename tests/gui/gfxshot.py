#!/usr/bin/env python3
"""Wait for "GFXTEST: drawn" in the debug log, then save the screen.

Usage: gfxshot.py debug.log monitor.sock out.ppm [timeout]
Runs next to QEMU (in the background); the monitor is the HMP socket.
"""
import os
import socket
import sys
import time


def main():
    log, sock, out = sys.argv[1:4]
    deadline = time.time() + (float(sys.argv[4]) if len(sys.argv) > 4 else 170)
    while time.time() < deadline:
        try:
            if b"GFXTEST: drawn" in open(log, "rb").read():
                break
        except OSError:
            pass
        time.sleep(0.2)
    else:
        return 1
    time.sleep(0.5)                     # let the display update
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    s.sendall(("screendump %s\n" % out).encode())
    time.sleep(1)
    s.close()
    return 0 if os.path.exists(out) else 1


if __name__ == "__main__":
    sys.exit(main())
