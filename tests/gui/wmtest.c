/*
 * WMTEST - mdgui window manager test (DJGPP).
 *
 * Three overlapping windows (bottom to top):
 *   Alpha (100,100 300x200) red, Beta (300,200 300x200) green,
 *   Gamma (600,120 250x180) blue
 * Logs to port E9h: "WMTEST: ready at X,Y" and "WMTEST: at X,Y" (the
 * mouse, on every move), clicks in the windows, and on q: "WMTEST: done Alpha=x,y Beta=x,y Gamma=x,y top=T".
 * The harness (tests/gui/wmdrive.py) drags Alpha by its title bar to
 * (300,350), clicks into Beta to raise it, takes a screenshot, presses q.
 */
#include <stdio.h>
#include <string.h>
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

static int handler(struct window *w, const struct event *e)
{
  char buf[96];

  if (e->type == EV_MOUSE_DOWN)
  {
    sprintf(buf, "WMTEST: click in %s at %d,%d\n", w->title, e->x, e->y);
    out(buf);
    return 1;
  }
  return 0;
}

static struct window *make(int x, int y, int w, int h, const char *title, color_t c)
{
  struct window *win = wm_window_new(x, y, w, h, title);
  char buf[64];

  draw_fill(win->client, 0, 0, win->client->w, win->client->h, c);
  sprintf(buf, "This is %s", title);
  draw_text(win->client, 12, 12, buf, RGB(255, 255, 255), TEXT_TRANSPARENT);
  win->handler = handler;
  wm_invalidate(win);
  return win;
}

int main(void)
{
  struct surface *s;
  char buf[160];
  int i, n;

  out("\nwindow manager test\n");
  if (gfx_open(1024, 768))
  {
    sprintf(buf, "WMTEST: failed: %s\n", gfx_error());
    out(buf);
    return 1;
  }
  s = gfx_screen();
  input_open();
  wm_init();
  make(100, 100, 300, 200, "Alpha", RGB(200, 60, 60));
  make(300, 200, 300, 200, "Beta", RGB(60, 180, 90));
  make(600, 120, 250, 180, "Gamma", RGB(60, 100, 200));
  wm_update();
  gfx_cursor_move(s->w / 2, s->h / 2);
  gfx_cursor_show(1);
  sprintf(buf, "WMTEST: ready at %d,%d\n", s->w / 2, s->h / 2);
  out(buf);

  /* wm_run, logging the mouse position (the harness steers by it) */
  for (;;)
  {
    struct event e;
    input_wait(&e);
    if (e.type == EV_MOUSE_MOVE)
    {
      sprintf(buf, "WMTEST: at %d,%d\n", e.x, e.y);
      out(buf);
    }
    wm_dispatch(&e);
    if (e.type == EV_KEY && (e.ascii == 'q' || e.ascii == 27))
      break;
    wm_update();
  }

  n = sprintf(buf, "WMTEST: done");
  for (i = 0; i < wm_count(); i++)
  {
    struct window *w = wm_window(i);
    n += sprintf(buf + n, " %s=%d,%d", w->title, w->x, w->y);
  }
  sprintf(buf + n, " top=%s\n", wm_top() ? wm_top()->title : "none");
  input_close();
  gfx_close();
  out(buf);
  return 0;
}
