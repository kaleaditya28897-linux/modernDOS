#!/usr/bin/env python3
"""Drive MOUSETEST through the QEMU monitor and check it.

Usage: mousedrive.py debug.log monitor.sock out.ppm
Waits for "MOUSETEST: ready", moves the mouse by (+120, +60) in small
steps, clicks the left button, waits for "MOUSETEST: click", saves a
screenshot and presses q. Runs next to QEMU (in the background); the
checks are done afterwards by mousecheck.py.
"""
import socket
import sys
import time

DX, DY, STEPS = 120, 60, 12


def text(path):
    try:
        return open(path, "rb").read().decode("latin-1")
    except OSError:
        return ""


def send(sock, cmd):
    for _ in range(20):                 # the monitor serves one client at a time
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(sock)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.2)
            s.close()
            return
        except OSError:
            time.sleep(0.3)


def wait(log, marker, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if marker in text(log):
            return True
        time.sleep(0.2)
    return False


def main():
    log, sock, out = sys.argv[1:4]
    if not wait(log, "MOUSETEST: ready", 290):
        return 1
    time.sleep(1)
    for _ in range(STEPS):
        send(sock, "mouse_move %d %d" % (DX // STEPS, DY // STEPS))
    time.sleep(0.5)
    send(sock, "mouse_button 1")
    send(sock, "mouse_button 0")
    if wait(log, "MOUSETEST: click", 10):
        time.sleep(0.5)
        send(sock, "screendump %s" % out)
        time.sleep(1)
    send(sock, "sendkey q")
    return 0


if __name__ == "__main__":
    sys.exit(main())
