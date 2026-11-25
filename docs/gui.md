# modernDOS desktop GUI (milestone M4)

The modernDOS desktop is a 32-bit DPMI program (DJGPP), with its own
original look and widget toolkit. It runs on both boot paths: from DOS
on a PC BIOS, and on UEFI PCs, where the MDVMM monitor's virtual BIOS
provides the graphics mode.

## Status

| Part | State |
|---|---|
| M4.1 graphics foundation | working: VBE linear framebuffer (32 bpp) on both boot paths, `mdgui` library: back buffer, clipped drawing, 8x16 text, present |
| M4.2 mouse and events | working: INT 33h on both boot paths (UEFI: virtual BIOS driver, USB mice; DOS: CTMOUSE), `mdgui` events and mouse cursor |
| M4.3 window system | working: windows with frames, title bars and close buttons, z-order, focus, moving, damage repaint (`wm.c`) |
| M4.4 widget toolkit | working: labels, buttons, checkboxes, text fields, lists, menu bar with drop-down menus, keyboard focus (`ui.c`) |
| M4.5 desktop shell | working: MDDESK with a panel (launcher, window list, clock), About, running DOS programs |

## Graphics modes

Programs use VBE 2.0+ (INT 10h AX=4Fxxh) for a 32 bpp direct color mode
with a linear framebuffer, mapped with DPMI 0800h.

- DOS boot: the real VBE BIOS (in QEMU: SeaBIOS VGA BIOS, mode 144h at
  1024x768).
- UEFI boot: the virtual BIOS (`mdvmm/core/bios.c`) offers VBE 3.0 with
  one mode, 141h: the GOP framebuffer as the firmware set it up (QEMU:
  1280x800). 4F00h/4F01h/4F02h/4F03h, 4F06h (get), 4F07h (start 0,0).
  While the mode is set the text console (`fbcon.c`) stops drawing;
  INT 10h AH=00h or 4F02h with mode 3 returns to text mode, and the
  console is redrawn.

## mdgui library

`gui/lib/`, built into `gui/build/mdgui.o` (one relocatable object: link
it like a .o). API in `gui/lib/mdgui.h`:

- `gfx_open(w, h)` picks the mode (w x h if there is one, else the
  largest that fits, else the smallest), maps the framebuffer and sets
  the mode; `gfx_close()` returns to text mode.
- Drawing goes to surfaces in normal memory: `gfx_screen()` is the back
  buffer. `gfx_present(x, y, w, h)` copies an area to the framebuffer,
  converting the 0x00RRGGBB colors to the mode's layout when needed.
- `draw_*`: pixel, fill, lines, frame, vertical gradient, blit, all
  clipped to the surface's clip rectangle (`surface_clip`).
- `draw_text`: the 8x16 code page 437 font (shared with the MDVMM text
  console, `mdvmm/core/font.c`).

## Input

`input_open()` finds the INT 33h driver, sets its range to the screen,
1:1 motion (function 0Fh, 8 mickeys per 8 pixels) and no double speed.
`input_poll()` / `input_wait()` return events: mouse move, button down
and up (from the press/release counters, functions 05h/06h, so short
clicks between polls are not lost), and keys (INT 16h AH=10h/11h: scan
code, character, shift flags). `input_wait()` idles with INT 2Fh
AX=1680h, so other DOS sessions run.

The mouse cursor is not in the back buffer: `gfx_cursor_move()` and
`gfx_present()` compose it over the presented rows.

Mouse drivers:

- DOS boot: any INT 33h driver; the tests use CTMOUSE (FreeDOS) with the
  PS/2 mouse. CTMOUSE accelerates fast motion.
- UEFI boot: the virtual BIOS has a driver (`mdvmm/core/mouse.c`) fed by
  USB boot protocol mice on xHCI root ports. DOS resets INT 33h while it
  starts, so `MDVMM` (no arguments, in AUTOEXEC.BAT) installs it through
  INT 2Fh AX=DE07h. In text mode the cursor is an inverted cell, drawn by
  the console. Functions 00h-08h, 0Bh, 0Fh, 1Bh, 21h, 24h; not yet the
  event handler (0Ch) or PS/2 mice.

## Window manager

`gui/lib/wm.c`. The desktop is one program; all its windows live in it,
much like the Windows 3.x GUI ran in one address space.

- A window (`struct window`) has an outer rectangle, a title, and a
  retained client surface the program draws into, then calls
  `wm_invalidate()`. `wm_window_new()`, `wm_window_close()`,
  `wm_window_move()`, `wm_raise()`.
- Repaint: changes add to a damaged rectangle (`wm_damage()`);
  `wm_update()` draws the desktop and the windows bottom to top, clipped
  to it, and presents only that area.
- Events (`wm_dispatch()`, or `wm_run()` as the whole loop): a mouse
  press raises and focuses the window under the pointer; on the title bar
  it starts a move, on the close button it sends `EV_CLOSE` (the window
  closes unless the handler takes it); elsewhere the window gets the
  event in client coordinates and keeps the mouse until the button is
  released. Keys go to the focused (top) window.
- Look: slate title bars, teal for the active window, a red close button,
  1-pixel dark frames, a dark blue-grey gradient desktop.

## Widgets

`gui/lib/ui.c`. `ui_new(window)` takes over the window's handler (events
no widget uses, and `EV_CLOSE`, go on to the previous one) and draws the
widgets into the window's client surface.

| Widget | Mouse | Keyboard | `action` |
|---|---|---|---|
| `ui_label` | - | - | - |
| `ui_button` | press and release on it | Enter, Space | clicked |
| `ui_check` | toggles | Space | toggled (`value`) |
| `ui_textfield` | places the caret | characters, Backspace, Del, Left/Right, Home/End | Enter |
| `ui_list` | selects; the scroll bar pages | Up/Down, Home/End, Enter | selected (`value`) |
| `ui_menubar` | opens a menu; an item chooses it | F10 opens the first menu; Up/Down, Enter, Esc | chosen (`menu`, `value`) |

Tab and Shift+Tab move the focus in creation order; the focused widget
is drawn in teal. Menus are popup windows (`wm_popup_new`): no title bar,
on top, closed by a mouse press outside them.

## Desktop shell

`gui/desk/desk.c`, built into `gui/build/MDDESK.EXE` (also on the
release image). `wm_desktop()` gives the shell a 30-pixel band at the top
that windows stay below, drawn after the desktop background, and the
events where no window is.

- Panel: the launcher button, a button per open window (the top one
  highlighted; a click raises the window), and a clock (HH:MM, checked
  while idle).
- Launcher (click, or F10): About modernDOS (display, boot type and
  sessions through the MDVMM API, free DPMI memory), Run... and Exit to
  DOS. Menus are `ui_popup()` popups.
- Run: the desktop calls `gfx_suspend()` (text mode, buffers kept), runs
  the command with `system()` (COMMAND.COM), waits for a key, then
  `gfx_resume()` sets the mode again and everything is redrawn.
- 32-bit DPMI programs run this way too: the program becomes a DPMI
  client started by the desktop's (the host nests them).

Two monitor fixes came with it: a DPMI client running a DOS program is no
longer torn down when that program exits (INT 21h AH=4Ch is only the
client's exit when DOS's current PSP is the client's), and the monitor's
own services (INT 2Fh AH=DEh, INT 15h AH=87h) now also answer interrupts
a DPMI client reflects to real mode, not only V86 INT instructions.

## Testing

`tests/gui/gfxtest.c` (GFXTEST) runs in the MDVMM test (DOS boot) and
the UEFI test. It draws a known pattern, logs `GFXTEST: drawn WxH`, and
returns to text mode after 3 seconds. `tests/gui/gfxshot.py` takes a
QEMU screendump when the pattern is up, `tests/gui/gfxcheck.py` checks
pixels: the corner boxes, the background, clipping, the gradient.
`SHOTS=dir` keeps the screenshots (gfx, mouse, wm .ppm).

`tests/gui/mousetest.c` (MOUSETEST) shows the cursor and logs clicks;
`tests/gui/mousedrive.py` moves the mouse by (+120, +60) through the QEMU
monitor, clicks, takes a screenshot and presses q; `mousecheck.py`
checks the click position (exact with the virtual BIOS driver, direction
only with CTMOUSE), the orange square and the cursor in the screenshot.

`tests/gui/wmtest.c` (WMTEST) opens three overlapping windows.
`wmdrive.py` drags one by its title bar and clicks another to raise it;
it steers the pointer closed loop by the positions WMTEST logs, because
QEMU only sends relative motion and CTMOUSE accelerates it. `wmcheck.py`
checks the final layout and z-order from the log and pixels in the
screenshot (the raised window covers the overlap, the moved window's old
place shows the desktop).

`tests/gui/uitest.c` (UITEST) is a dialog with every widget.
`uidrive.py` types into the text field, toggles the checkbox with Tab and
Space, picks a list item, clicks OK, and quits through File/Quit with the
keyboard; `uicheck.py` checks the values the OK button logged and the
checkbox, list and text field in the screenshot. `guidrive.py` holds the
shared closed loop mouse steering.

`MDDESK /TEST` logs positions and events; `deskdrive.py` opens About
from the launcher, runs `A:\V86CHK` through Run (it logs a second time)
and returns to the desktop with a key, then exits through the launcher;
`deskcheck.py` checks the clock, the boot type the About box got from the
MDVMM API, the window list, the program run and the redrawn screen.

The DJGPP test programs are linked
with `-s`: unstripped, they no longer fit on the 1.44 MB test floppy.
