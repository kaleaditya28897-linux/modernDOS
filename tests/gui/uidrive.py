#!/usr/bin/env python3
"""Drive UITEST through the QEMU monitor (see uitest.c).

Usage: uidrive.py debug.log monitor.sock out.ppm
"""
import re
import sys
import time

from guidrive import Mouse, send, text, type_text, wait


def main():
    log, sock, out = sys.argv[1:4]
    if not wait(log, "UITEST: ready", 290):
        return 1
    w = {m[0]: (int(m[1]), int(m[2]))
         for m in re.findall(r"UITEST: widget (\w+) (\d+),(\d+)", text(log))}
    mouse = Mouse(sock, log, "UITEST")
    time.sleep(1)
    mouse.click(*w["text"])                     # the text field
    type_text(sock, "hello")
    send(sock, "sendkey tab")                   # focus: the checkbox
    send(sock, "sendkey spc")                   # on
    mouse.click(*w["item3"])
    mouse.click(*w["ok"])
    wait(log, "UITEST: ok", 10)
    time.sleep(1)
    send(sock, "screendump %s" % out)
    time.sleep(1)
    mouse.click(*w["file"])                     # File menu
    time.sleep(0.5)
    send(sock, "sendkey down")                  # Quit
    send(sock, "sendkey ret")
    return 0


if __name__ == "__main__":
    sys.exit(main())
