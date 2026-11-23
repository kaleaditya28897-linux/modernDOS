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
| M4.3 window system | planned: windows, z-order, moving, damage repaint |
| M4.4 widget toolkit | planned: buttons, text fields, lists, menus, layout, theme |
| M4.5 desktop shell | planned: launcher, clock, starting DOS programs |

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

## Testing

`tests/gui/gfxtest.c` (GFXTEST) runs in the MDVMM test (DOS boot) and
the UEFI test. It draws a known pattern, logs `GFXTEST: drawn WxH`, and
returns to text mode after 3 seconds. `tests/gui/gfxshot.py` takes a
QEMU screendump when the pattern is up, `tests/gui/gfxcheck.py` checks
pixels: the corner boxes, the background, clipping, the gradient.
`GFXSHOT=file.ppm` keeps the screenshot.

`tests/gui/mousetest.c` (MOUSETEST) shows the cursor and logs clicks;
`tests/gui/mousedrive.py` moves the mouse by (+120, +60) through the QEMU
monitor, clicks, takes a screenshot and presses q; `mousecheck.py`
checks the click position (exact with the virtual BIOS driver, direction
only with CTMOUSE), the orange square and the cursor in the screenshot.
