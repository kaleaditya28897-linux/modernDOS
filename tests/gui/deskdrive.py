#!/usr/bin/env python3
"""Drive MDDESK /TEST through the QEMU monitor (see gui/desk/desk.c).

Usage: deskdrive.py debug.log monitor.sock out.ppm
Opens About from the launcher, opens Run, runs V86CHK (a DOS program:
it logs again), returns to the desktop with a key, takes a screenshot;
then the file manager: creates, renames, opens and deletes a folder on
A:; then opens A:\Created On Linux.txt in the editor from Files, adds to
it, saves with Ctrl+S, closes; exits through the launcher.
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
    mouse.click(*point(log, "launcher"))
    time.sleep(0.5)
    mouse.click(*point(log, "exit"))            # Exit to DOS
    return 0


if __name__ == "__main__":
    sys.exit(main())
