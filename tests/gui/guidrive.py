#!/usr/bin/env python3
"""Shared helpers for driving GUI tests through the QEMU monitor.

QEMU sends relative mouse motion, which mouse drivers scale and accelerate
(CTMOUSE does), so the pointer is steered closed loop: the test program
logs every position ("<PREFIX>: at X,Y", first "<PREFIX>: ready at X,Y")
and moves are repeated until it is there.
"""
import re
import time

from mousedrive import send, text, wait  # noqa: F401  (re-exported)

STEP = 40


class Mouse:
    def __init__(self, sock, log, prefix):
        self.sock, self.log = sock, log
        self.pattern = re.compile(prefix + r": (?:ready )?at (\d+),(\d+)")

    def pos(self):
        found = self.pattern.findall(text(self.log))
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

    def click(self, x, y):
        self.go(x, y)
        self.button(True)
        self.button(False)


KEYS = {" ": "spc", "\n": "ret", "\t": "tab", ".": "dot", ",": "comma", "-": "minus"}


def type_text(sock, s):
    for c in s:
        k = KEYS.get(c, c)
        if c.isupper():
            k = "shift-" + c.lower()
        send(sock, "sendkey " + k)
        time.sleep(0.1)
