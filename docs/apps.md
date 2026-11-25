# modernDOS applications (milestone M5)

The applications are part of the desktop program, MDDESK (`gui/desk/`):
the DPMI host runs one 32-bit client at a time, so they are modules of
it, each in its own windows. They are started from the launcher.

## Status

| Part | State |
|---|---|
| M5.1 file manager | working: browse folders with long file names, open folders and programs, new folder, rename, delete |
| M5.2 text editor | working: multi-line editing widget, editor with File menu, Ctrl+S, opened from Files |
| M5.3 settings | working: background, accent colour, 12/24-hour clock, mouse speed; live, saved in MDDESK.CFG |
| M5.4 terminal (a DOS session in a window) | working on UEFI boots: a job slot's session runs COMMAND.COM, its text screen is drawn in the window, the window's keys are typed into it (`gui/desk/term.c`) |

## Files

`gui/desk/files.c`. A window per view, opened at the root of the
current drive:

- Path: type a folder (or a drive, `C:`) and press Enter.
- Buttons: Up, Refresh, New folder, Rename, Delete, Open.
- The list: folders first, then files, by name (case-insensitive), with
  the size and date; `..` in subfolders. Enter or a double click opens:
  a folder is entered, a program (`.EXE`, `.COM`, `.BAT`) runs through
  the desktop (text mode, back on a key), any other file opens in the
  editor.
- New folder and Rename ask with `ui_ask_text()`; Delete asks with
  `ui_confirm()` (folders must be empty).

Toolkit additions for it: lists have an `activate` action (Enter, double
click) besides `action` (selection), and the dialogs `ui_ask_text()` and
`ui_confirm()`.

## Editor

`gui/desk/editor.c`, a window per file, from the launcher (a new text)
or from Files.

- File menu: New, Open..., Save, Save as..., Close; Ctrl+S saves.
- Status line: line, column, modified. The title shows `*` while there
  are unsaved changes; closing then asks first.
- Files are read with LF or CRLF line ends (up to 1 MB) and written with
  CRLF.

The text is the toolkit's `ui_edit()` widget: a line array; typing,
Enter splits a line, Backspace and Delete join lines, arrows,
Home/End, PgUp/PgDn, Ctrl+Home/End, Tab inserts spaces (to a multiple
of 4); the mouse places the caret, the scroll bar pages.
`edit_set_text()`, `edit_get_text()` (CRLF), `edit_modified()`,
`edit_caret()`.

## Settings

`gui/desk/settings.c`, from the launcher. Changes apply at once:

- Background: Slate (default), Ocean, Forest, Plum, Charcoal (gradients).
- Accent colour: Teal (default), Blue, Orange, Purple, Green - active
  title bars, focus, selection, the panel's highlights.
- 24-hour clock (off: 12-hour with AM/PM).
- Mouse speed: Slow, Normal, Fast (INT 33h function 0Fh, 16/8/4 mickeys
  per 8 pixels).

Save writes `MDDESK.CFG` (`key=value` lines) next to `MDDESK.EXE`; the
desktop reads it when it starts. On UEFI boots `A:` is a RAM copy, so a
saved file lasts until the next boot.

The colours are `theme` in mdgui (`theme.c`); `wm_redraw_all()` sends
`EV_REDRAW` to every window so retained contents are drawn again in the
new colours.

## Testing

`tests/gui/deskdrive.py` (MDDESK /TEST, both boot paths), after the
desktop steps: opens Files, creates `Test Folder`, renames it to
`Renamed Dir`, opens it (only `..` inside), goes Up, deletes it. The
desktop logs every listing and operation; `deskcheck.py` checks them.
Then it opens `A:\Created On Linux.txt` from Files in the editor, adds
to the line and a second line, saves with Ctrl+S (53 bytes, CRLF) and
closes. On DOS boots the MDVMM test also reads the file back from the
disk image afterwards. Then Settings: Ocean, Orange, 12-hour clock, Save;
AUTOEXEC.BAT then starts `MDDESK /TEST /EXIT`, which must load the same
settings. `files.ppm`, `editor.ppm` and `settings.ppm` (with
`SHOTS=dir`) show the applications.

## Terminal

`gui/desk/term.c`. Launcher > Terminal opens a window with a DOS prompt.

- MDVMM keeps the other DOS sessions running beside the desktop. The
  terminal takes a *job slot*: a session started by
  `SSHSLOT.COM` before the desktop (AUTOEXEC.BAT does), which waits for
  work. The same slots serve remote shells of the SSH server.
- The window shows the session's 80x25 text screen (monitor call INT 2Fh
  AX=DE08h, about 20 times a second, changed cells only) in CGA colours
  with the cursor; keys go into the session's BIOS keyboard buffer
  (AX=DE09h). `EXIT` closes the window; closing the window sends Ctrl+C
  and `EXIT` to the shell.
- On both boots: a session that is not on the display writes its text
  into its own screen memory (on DOS boots the monitor keeps it from the
  video hardware). A program that sets a graphics mode waits until its
  session is on the display (Ctrl+Alt+F2..).
- Programs in the terminal can be 32-bit DPMI programs: every session has
  its own DPMI client.

## Browser

`gui/desk/browser.c`. Launcher > Browser. An address field with Go and
Back, the page below.

- Pages are laid out as text in the window's fixed-width font: headings,
  paragraphs, line breaks, lists, links (underlined, click to follow),
  preformatted text, rules, table rows, placeholders for images and form
  fields, character entities, UTF-8 shown in code page 437. Scripts and
  style sheets are skipped.
- Keys in the page: Up, Down, PgUp, PgDn, Space, Home, End; Backspace
  goes back.
- `http://` and `https://` (TLS 1.2 with BearSSL, `net/pkg/tls.c`; server
  certificates are checked against Mozilla's list of authorities, built
  in). No scripts run, so pages that scripts build stay empty; the start
  page lists places that read well as text.
- The network is Watt-32 through the packet driver (`PKTDRV`, loaded by
  AUTOEXEC.BAT before the desktop); loading a page blocks the desktop
  until it is there.
- The page view is a `UI_CANVAS` widget (`ui_canvas()` in mdgui): an
  area a program draws and handles itself.
