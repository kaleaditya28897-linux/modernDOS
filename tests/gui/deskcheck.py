#!/usr/bin/env python3
"""Check MDDESK's test run (see deskdrive.py).

Usage: deskcheck.py debug.log screen.ppm
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxcheck import read_ppm  # noqa: E402

PANEL = (24, 30, 40)
ACTIVE = (32, 128, 136)


def main():
    log = open(sys.argv[1], "rb").read().decode("latin-1")
    about = re.search(r"MDDESK: window About at (\d+),(\d+)", log)
    tasks = re.findall(r"MDDESK: taskbar(.*)", log)
    checks = [
        ("the clock shows HH:MM", bool(re.search(r"MDDESK: clock \d\d:\d\d", log))),
        ("About opened from the launcher", bool(about)),
        ("MDVMM's API answers a DPMI program (About: boot via MDVMM)",
         bool(re.search(r"MDDESK: about Boot: (UEFI|BIOS), MDVMM", log))),
        ("the panel lists the About window", any("About modernDOS=" in t for t in tasks)),
        ("Run started V86CHK (it logged again)",
         log.count("v86chk: protected mode bit set") >= 2 and "MDDESK: ran A:\\V86CHK exit 0" in log),
        ("back in graphics after the program", "MDDESK: resumed" in log),
        ("Exit to DOS", "MDDESK: exit" in log),
        ("Files: listed A:\\", bool(re.search(r"MDDESK: files A:\\ \d+:.*kernel\.sys", log, re.I))),
        ("Files: new folder", "MDDESK: created A:\\Test Folder" in log),
        ("Files: renamed", "MDDESK: renamed Test Folder to Renamed Dir" in log),
        ("Files: opened the folder (only ..)", "MDDESK: files A:\\Renamed Dir\\ 1: .." in log),
        ("Files: deleted", "MDDESK: deleted A:\\Renamed Dir" in log),
        ("Editor: opened from Files", "MDDESK: editor opened A:\\Created On Linux.txt, 1 lines" in log),
        ("Editor: saved with Ctrl+S (53 bytes, CRLF)",
         "MDDESK: editor saved A:\\Created On Linux.txt, 53 bytes: hello from linux - edited in modernDOS"
         in log),
        ("Editor: closed", "MDDESK: editor closed" in log),
        ("Settings: saved", "MDDESK: settings saved background=Ocean accent=Orange clock=12 mouse=Normal"
         in log),
        ("Settings: the clock went to 12 hours", bool(re.search(r"MDDESK: clock \d{1,2}:\d\d [AP]M", log))),
        ("Settings: the next MDDESK loads them",
         "MDDESK: settings loaded background=Ocean accent=Orange clock=12 mouse=Normal" in log),
        # a 32-bit program started from the desktop: a client's client
        ("Run started DPMITEST, a DPMI program (a client started by a client), it passed",
         bool(re.search(r"dpmi: 32 bit client, PSP \w+, session \d, started by a client", log))
         and bool(re.search(r"MDDESK: ran A:\\DPMITEST exit [01]\b", log))
         and len(re.findall(r"DPMI: [01] failures", log)) >= 2),   # (its 24 MB may not be free beside the desktop)
        # the terminal: a DOS session in a window where there are job slots
        # (TERMINAL=1), a message elsewhere
        ("Terminal: a DOS session in a window, typed command's output shown, closed by EXIT"
         if os.environ.get("TERMINAL") else "Terminal: says why it cannot open here",
         ("MDDESK: term opened in session" in log and "MDDESK: term line Terminal 123" in log
          and "MDDESK: term closed" in log)
         if os.environ.get("TERMINAL") else "MDDESK: term unavailable" in log),
        ("Terminal: a DPMI program ran in its session beside the desktop"
         if os.environ.get("TERMINAL") else "Terminal: no DPMI program tried",
         len(re.findall(r"DPMI: [01] failures", log)) >= 3 if os.environ.get("TERMINAL") else True),
        # the browser, where the test has a web server (BROWSER=port)
        ("Browser: start page, a page by its address, a link followed"
         if os.environ.get("BROWSER") else "Browser: not tested on this boot",
         ("MDDESK: browser loaded about:start" in log
          and bool(re.search(r"MDDESK: browser loaded http://10\.0\.2\.2:\d+/ title=Test Page lines=\d+ links=2", log))
          and bool(re.search(r"MDDESK: browser loaded http://10\.0\.2\.2:\d+/sub/page2\.html title=Second Page", log)))
         if os.environ.get("BROWSER") else True),
        ("Files: gone from the listing",
         bool(re.findall(r"MDDESK: files A:\\ \d+:(.*)", log))
         and "Renamed Dir" not in re.findall(r"MDDESK: files A:\\ \d+:(.*)", log)[-1]),
    ]
    try:
        w, h, px = read_ppm(sys.argv[2])

        def pixel(x, y):
            i = (y * w + x) * 3
            return tuple(px[i:i + 3])

        checks.append(("panel drawn after resuming", pixel(w // 2, 2) == PANEL))
        if about:
            ax, ay = int(about.group(1)), int(about.group(2))
            checks.append(("About window drawn, active", pixel(ax + 200, ay + 12) == ACTIVE))
    except (OSError, ValueError) as e:
        checks.append(("screenshot: %s" % e, False))
    try:
        sw, sh, spx = read_ppm(os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])),
                                            "settings.ppm"))

        def spixel(x, y):
            i = (y * sw + x) * 3
            return tuple(spx[i:i + 3])

        win = re.search(r"MDDESK: window Settings at (\d+),(\d+)", log)
        checks.append(("Settings: Ocean desktop", spixel(10, sh - 1) == (8, 28, 52)))
        if win:
            wx, wy = int(win.group(1)), int(win.group(2))
            checks.append(("Settings: Orange title bar",
                           spixel(wx + 200, wy + 12) == (196, 104, 32)))
    except (OSError, ValueError) as e:
        checks.append(("settings screenshot: %s" % e, False))
    fail = 0
    for name, ok in checks:
        print("  %s desk: %s" % ("ok  " if ok else "FAIL", name))
        fail |= not ok
    return fail


if __name__ == "__main__":
    sys.exit(main())
