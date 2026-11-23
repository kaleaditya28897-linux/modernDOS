# modernDOS applications (milestone M5)

The applications are part of the desktop program, MDDESK (`gui/desk/`):
the DPMI host runs one 32-bit client at a time, so they are modules of
it, each in its own windows. They are started from the launcher.

## Status

| Part | State |
|---|---|
| M5.1 file manager | working: browse folders with long file names, open folders and programs, new folder, rename, delete |
| M5.2 text editor | planned |
| M5.3 settings | planned |
| M5.4 terminal (a DOS session in a window) | planned: needs MDVMM sessions while a DPMI client runs, and a session screen/keyboard API |

## Files

`gui/desk/files.c`. A window per view, opened at the root of the
current drive:

- Path: type a folder (or a drive, `C:`) and press Enter.
- Buttons: Up, Refresh, New folder, Rename, Delete, Open.
- The list: folders first, then files, by name (case-insensitive), with
  the size and date; `..` in subfolders. Enter or a double click opens:
  a folder is entered, a program (`.EXE`, `.COM`, `.BAT`) runs through
  the desktop (text mode, back on a key).
- New folder and Rename ask with `ui_ask_text()`; Delete asks with
  `ui_confirm()` (folders must be empty).

Toolkit additions for it: lists have an `activate` action (Enter, double
click) besides `action` (selection), and the dialogs `ui_ask_text()` and
`ui_confirm()`.

## Testing

`tests/gui/deskdrive.py` (MDDESK /TEST, both boot paths), after the
desktop steps: opens Files, creates `Test Folder`, renames it to
`Renamed Dir`, opens it (only `..` inside), goes Up, deletes it. The
desktop logs every listing and operation; `deskcheck.py` checks them.
`files.ppm` (with `SHOTS=dir`) shows the file manager.
