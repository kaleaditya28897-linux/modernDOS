#!/usr/bin/env python3
"""Drive WMTEST through the QEMU monitor. The pointer is steered
closed loop by the positions WMTEST logs (guidrive.py).

Usage: wmdrive.py debug.log monitor.sock out.ppm
Drags Alpha's title bar from (160,112) to (360,362), clicks into Beta at
(450,280), saves a screenshot and presses q.
"""
import sys
import time

from guidrive import Mouse, send, wait


def main():
    log, sock, out = sys.argv[1:4]
    if not wait(log, "WMTEST: ready", 290):
        return 1
    mouse = Mouse(sock, log, "WMTEST")
    time.sleep(1)
    mouse.go(160, 112)                  # Alpha's title bar
    mouse.button(True)
    mouse.go(360, 362)                  # Alpha to (300,350)
    mouse.button(False)
    mouse.go(450, 280)                  # into Beta: raises it
    mouse.button(True)
    mouse.button(False)
    wait(log, "WMTEST: click in Beta", 10)
    time.sleep(1)
    send(sock, "screendump %s" % out)
    time.sleep(1)
    send(sock, "sendkey q")
    return 0


if __name__ == "__main__":
    sys.exit(main())
