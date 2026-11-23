#!/usr/bin/env python3
"""Drive WMTEST through the QEMU monitor.

QEMU sends relative motion, which mouse drivers scale and accelerate
(CTMOUSE does), so the pointer is steered closed loop: WMTEST logs every
position ("WMTEST: at X,Y") and moves are repeated until it is there.

Usage: wmdrive.py debug.log monitor.sock out.ppm
Drags Alpha's title bar from (160,112) to (360,362), clicks into Beta at
(450,280), saves a screenshot and presses q.
"""
import re
import sys
import time

from mousedrive import send, text, wait

STEP = 40


class Mouse:
    def __init__(self, sock, log):
        self.sock, self.log = sock, log

    def pos(self):
        found = re.findall(r"WMTEST: (?:ready )?at (\d+),(\d+)", text(self.log))
        return int(found[-1][0]), int(found[-1][1])

    def go(self, x, y):
        """Steer to x,y; the step halves on an axis when it overshoots
        (accelerating drivers). Exact if possible, else within 3 pixels."""
        step = [STEP, STEP]
        last = [0, 0]
        for i in range(300):
            cx, cy = self.pos()
            err = [x - cx, y - cy]
            if err == [0, 0] or (i > 150 and abs(err[0]) <= 3 and abs(err[1]) <= 3):
                return True
            for a in (0, 1):
                if err[a] * last[a] < 0:        # overshot: smaller steps
                    step[a] = max(1, step[a] // 2)
                last[a] = err[a]
            dx = max(-step[0], min(step[0], err[0]))
            dy = max(-step[1], min(step[1], err[1]))
            send(self.sock, "mouse_move %d %d" % (dx, dy))
            time.sleep(0.15)
        return False

    def button(self, down):
        send(self.sock, "mouse_button %d" % (1 if down else 0))
        time.sleep(0.3)


def main():
    log, sock, out = sys.argv[1:4]
    if not wait(log, "WMTEST: ready", 290):
        return 1
    mouse = Mouse(sock, log)
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
