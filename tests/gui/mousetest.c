/*
 * MOUSETEST - mdgui input test (DJGPP).
 *
 * Graphics mode with the mouse cursor in the middle; logs to port E9h:
 *   "MOUSETEST: ready WxH at X,Y"   - start position
 *   "MOUSETEST: click at X,Y"       - left button pressed (an orange
 *                                     square is drawn there)
 *   "MOUSETEST: done"               - after 'q' or Esc
 * The harness (tests/gui/mousedrive.py) moves the mouse, clicks, takes a
 * screenshot and presses q. Exit code 1 without a mouse or a click.
 */
#include <stdio.h>
#include <pc.h>
#include "mdgui.h"

static void out(const char *s)
{
  for (; *s; s++)
  {
    if (*s == '\n')
      outportb(0xe9, '\r');
    outportb(0xe9, *s);
  }
}

int main(void)
{
  struct surface *s;
  struct event e;
  char buf[96];
  int clicks = 0, moves = 0, mouse;

  out("\nmouse test\n");
  if (gfx_open(1024, 768))
  {
    sprintf(buf, "MOUSETEST: failed: %s\n", gfx_error());
    out(buf);
    return 1;
  }
  s = gfx_screen();
  draw_gradient(s, 0, 0, s->w, s->h, RGB(20, 40, 70), RGB(60, 90, 120));
  draw_text(s, 20, 20, "Mouse test: click, then press q", RGB(255, 255, 255), TEXT_TRANSPARENT);
  gfx_present_all();
  mouse = input_open();
  gfx_cursor_move(s->w / 2, s->h / 2);
  gfx_cursor_show(1);
  sprintf(buf, "MOUSETEST: ready %dx%d at %d,%d, mouse %s\n", s->w, s->h, s->w / 2, s->h / 2,
          mouse ? "yes" : "no");
  out(buf);

  for (;;)
  {
    input_wait(&e);
    if (e.type == EV_MOUSE_MOVE)
    {
      gfx_cursor_move(e.x, e.y);
      moves++;
    }
    else if (e.type == EV_MOUSE_DOWN && e.button == MB_LEFT)
    {
      draw_fill(s, e.x - 10, e.y - 10, 20, 20, RGB(255, 140, 0));
      gfx_present(e.x - 10, e.y - 10, 20, 20);
      sprintf(buf, "MOUSETEST: click at %d,%d\n", e.x, e.y);
      out(buf);
      clicks++;
    }
    else if (e.type == EV_KEY && (e.ascii == 'q' || e.ascii == 27))
      break;
  }
  input_close();
  gfx_close();
  sprintf(buf, "MOUSETEST: done, %d moves, %d clicks\n", moves, clicks);
  out(buf);
  return !(mouse && clicks);
}
