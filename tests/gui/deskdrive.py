#!/usr/bin/env python3
"""Drive MDDESK /TEST through the QEMU monitor (see gui/desk/desk.c).

Usage: deskdrive.py debug.log monitor.sock out.ppm
Opens About from the launcher, opens Run, runs V86CHK (a DOS program:
it logs again), returns to the desktop with a key, takes a screenshot,
and exits through the launcher.
"""
import re
import sys
import time

from guidrive import Mouse, send, text, type_text, wait


def point(log, name):
    found = re.findall(r"MDDESK: (?:widget|item) %s (\d+),(\d+)" % re.escape(name), text(log))
    return int(found[-1][0]), int(found[-1][1])


def main():
    log, sock, out = sys.argv[1:4]
    if not wait(log, "MDDESK: ready", 290):
        return 1
    mouse = Mouse(sock, log, "MDDESK")
    time.sleep(1)
    mouse.click(*point(log, "launcher"))
    wait(log, "MDDESK: item 2", 5)
    mouse.click(*point(log, "0"))               # About modernDOS
    wait(log, "MDDESK: window About", 5)
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "1"))               # Run...
    wait(log, "MDDESK: widget runbutton", 5)
    mouse.click(*point(log, "runfield"))
    type_text(sock, "A:\\V86CHK")      # full path: DJTEST left the cwd elsewhere
    send(sock, "sendkey ret")
    wait(log, "MDDESK: ran", 30)
    time.sleep(1)
    send(sock, "sendkey spc")                   # back to the desktop
    wait(log, "MDDESK: resumed", 10)
    time.sleep(1.5)
    send(sock, "screendump %s" % out)
    time.sleep(1)
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "2"))               # Exit to DOS
    return 0


if __name__ == "__main__":
    sys.exit(main())
