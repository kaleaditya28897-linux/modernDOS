#!/usr/bin/env python3
"""Drive MDDESK /TEST through the QEMU monitor (see gui/desk/desk.c).

Usage: deskdrive.py debug.log monitor.sock out.ppm
Opens About from the launcher, opens Run, runs V86CHK (a DOS program:
it logs again), returns to the desktop with a key, takes a screenshot;
then the file manager: creates, renames, opens and deletes a folder on
A:; then opens A:\Created On Linux.txt in the editor from Files, adds to
it, saves with Ctrl+S, closes; then Settings: Ocean, Orange, 12-hour
clock, Save; exits through the launcher. (AUTOEXEC then starts MDDESK
/TEST /EXIT, which must load those settings.)
"""
import os
import re
import sys
import time

from guidrive import Mouse, send, text, type_text, wait


def point(log, name, kind="(?:widget|item)"):
    found = re.findall(r"MDDESK: %s %s (\d+),(\d+)" % (kind, re.escape(name)), text(log))
    return int(found[-1][0]), int(found[-1][1])


def file_item(log, name):
    return point(log, name, "fileitem")


def files(mouse, sock, log, shot):
    """The file manager: create, rename, open, leave, delete a folder."""
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "files"))
    wait(log, "MDDESK: window Files", 10)
    time.sleep(0.5)
    mouse.click(*point(log, "files_newfolder"))
    wait(log, "MDDESK: widget dialogfield", 5)
    # typing at once lost the first keys in about one run of four on DOS
    # boots ("Folder" instead of "Test Folder"); not explained yet
    time.sleep(0.5)
    type_text(sock, "Test Folder\n")
    wait(log, "MDDESK: created", 10)
    time.sleep(0.5)
    mouse.click(*file_item(log, "Test Folder"))
    mouse.click(*point(log, "files_rename"))
    time.sleep(0.5)
    for _ in range(len("Test Folder")):
        send(sock, "sendkey backspace")
    type_text(sock, "Renamed Dir\n")
    wait(log, "MDDESK: renamed", 10)
    time.sleep(1)
    send(sock, "screendump %s" % shot)
    time.sleep(1)
    mouse.click(*file_item(log, "Renamed Dir"))
    send(sock, "sendkey ret")                   # open the folder
    wait(log, "MDDESK: files A:\\Renamed Dir\\", 10)
    time.sleep(0.5)
    mouse.click(*point(log, "files_up"))
    time.sleep(1)
    mouse.click(*file_item(log, "Renamed Dir"))
    mouse.click(*point(log, "files_delete"))
    wait(log, "MDDESK: widget confirmyes", 5)
    time.sleep(0.3)
    send(sock, "sendkey ret")                   # Yes
    wait(log, "MDDESK: deleted", 10)


def editor(mouse, sock, log, shot):
    """Open a text file from Files in the editor, change it, save, close."""
    mouse.click(*file_item(log, "Created On Linux.txt"))
    send(sock, "sendkey ret")
    if not wait(log, "MDDESK: editor opened", 10):
        return
    m = re.findall(r"MDDESK: window Editor at (\d+),(\d+)", text(log))[-1]
    time.sleep(0.5)
    send(sock, "sendkey end")
    type_text(sock, " - edited in modernDOS\nsecond line")
    send(sock, "sendkey ctrl-s")
    wait(log, "MDDESK: editor saved", 10)
    time.sleep(1)
    send(sock, "screendump %s" % shot)
    time.sleep(1)
    x, y = int(m[0]), int(m[1])
    mouse.click(x + 660 - 22 + 9, y + 12)       # the close button
    wait(log, "MDDESK: editor closed", 10)


def settings(mouse, sock, log, shot):
    """Settings: Ocean background, Orange accent, 12-hour clock, save."""
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "settings"))
    if not wait(log, "MDDESK: window Settings", 10):
        return
    time.sleep(0.5)
    mouse.click(*point(log, "bg_Ocean"))
    mouse.click(*point(log, "accent_Orange"))
    mouse.click(*point(log, "settings_clock24"))
    mouse.click(*point(log, "settings_save"))
    wait(log, "MDDESK: settings saved", 10)
    time.sleep(1)
    send(sock, "screendump %s" % shot)
    time.sleep(1)
    mouse.click(*point(log, "settings_close"))
    time.sleep(0.5)


def terminal(mouse, sock, log, shot):
    """The terminal: a DOS session in a window (UEFI boots with job slots;
    elsewhere it says why not)."""
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "terminal"))
    if not wait(log, "MDDESK: term opened", 5):
        return                                  # "term unavailable": a message window
    if not wait(log, "MDDESK: term ready", 30):
        return
    time.sleep(0.5)
    type_text(sock, "ECHO Terminal 123\n")
    wait(log, "MDDESK: term line Terminal 123", 20)
    time.sleep(1)
    send(sock, "screendump %s" % shot)
    time.sleep(1)
    type_text(sock, "EXIT\n")
    wait(log, "MDDESK: term closed", 20)
    time.sleep(0.5)


def browser(mouse, sock, log, shot):
    """The browser (BROWSER=port: a web server at 10.0.2.2:port): the start
    page, a page by address, a link."""
    port = os.environ.get("BROWSER")
    if not port:
        return
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "browser"))
    if not wait(log, "MDDESK: browser loaded about:start", 10):
        return
    time.sleep(0.5)
    mouse.click(*point(log, "browser_addr"))
    for _ in range(len("about:start")):
        send(sock, "sendkey backspace")
    type_text(sock, "10.0.2.2:%s/\n" % port)
    if not wait(log, "title=Test Page", 60):
        return
    time.sleep(1)
    found = re.findall(r"MDDESK: browser link 1 at (\d+),(\d+)", text(log))
    if found:
        mouse.click(int(found[-1][0]), int(found[-1][1]))
        wait(log, "title=Second Page", 60)
    time.sleep(1)
    send(sock, "screendump %s" % shot)
    time.sleep(1)


def main():
    log, sock, out = sys.argv[1:4]
    if not wait(log, "MDDESK: ready", 290):
        return 1
    mouse = Mouse(sock, log, "MDDESK")
    time.sleep(1)
    mouse.click(*point(log, "launcher"))
    wait(log, "MDDESK: item exit", 5)
    mouse.click(*point(log, "about"))           # About modernDOS
    wait(log, "MDDESK: window About", 5)
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "run"))             # Run...
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
    files(mouse, sock, log, os.path.join(os.path.dirname(out), "files.ppm"))
    time.sleep(1)
    editor(mouse, sock, log, os.path.join(os.path.dirname(out), "editor.ppm"))
    time.sleep(1)
    settings(mouse, sock, log, os.path.join(os.path.dirname(out), "settings.ppm"))
    time.sleep(1)
    terminal(mouse, sock, log, os.path.join(os.path.dirname(out), "term.ppm"))
    time.sleep(1)
    browser(mouse, sock, log, os.path.join(os.path.dirname(out), "browser.ppm"))
    time.sleep(1)
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "exit"))            # Exit to DOS
    return 0


if __name__ == "__main__":
    sys.exit(main())
