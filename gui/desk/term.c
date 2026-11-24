/*
 * MDDESK Terminal - a DOS session in a window (M5.4).
 *
 * The desktop is a DPMI program; MDVMM keeps the other DOS sessions
 * running beside it. A terminal window takes one of the job slots
 * (sessions started by SSHSLOT.COM before the desktop), runs COMMAND.COM
 * in it, shows that session's 80x25 text screen in the window and types
 * the window's keys into the session's keyboard buffer (monitor calls
 * INT 2Fh AX=DE08h..DE0Dh). EXIT in the shell closes the window; closing
 * the window sends Ctrl+C and EXIT.
 *
 * UEFI boots only: there MDVMM's own BIOS draws a background session's
 * text into that session's screen memory. On a DOS boot the machine's
 * video BIOS would write to the real display.
 * Programs in the terminal are real mode DOS programs (the desktop is the
 * one DPMI client).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include "desk.h"

#define COLS 80
#define ROWS 25
#define MAX_TERMS 3

struct term {
  struct window *win;
  int slot;
  unsigned char shown[COLS * ROWS * 2];
  int have;                             /* shown[] is valid */
  int row, col, cursor_on;
  int ready;                            /* the shell's prompt was seen (tests) */
};

static struct term terms[MAX_TERMS];
static int leaving[8];                  /* slots of closed windows, to end */
static clock_t last_poll;

static const color_t cga[16] = {
  RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0xaa), RGB(0x00, 0xaa, 0x00), RGB(0x00, 0xaa, 0xaa),
  RGB(0xaa, 0x00, 0x00), RGB(0xaa, 0x00, 0xaa), RGB(0xaa, 0x55, 0x00), RGB(0xaa, 0xaa, 0xaa),
  RGB(0x55, 0x55, 0x55), RGB(0x55, 0x55, 0xff), RGB(0x55, 0xff, 0x55), RGB(0x55, 0xff, 0xff),
  RGB(0xff, 0x55, 0x55), RGB(0xff, 0x55, 0xff), RGB(0xff, 0xff, 0x55), RGB(0xff, 0xff, 0xff),
};

/* ---------------------------------------------------------------- */
/* the monitor's session calls                                      */

static int api(__dpmi_regs *r, int al)
{
  r->h.ah = 0xde;
  r->h.al = (unsigned char)al;
  __dpmi_int(0x2f, r);
  return !(r->x.flags & 1);
}

static int uefi_boot(void)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  return api(&r, 0x15) && (r.x.ax & 1);
}

static int job_start(void)
{
  __dpmi_regs r;
  char empty = 0;

  memset(&r, 0, sizeof(r));
  dosmemput(&empty, 1, __tb);
  r.x.ds = __tb >> 4;
  r.x.si = __tb & 15;
  return api(&r, 0x0c) ? r.x.ax : -1;
}

static int job_state(int slot)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  r.h.bl = (unsigned char)slot;
  return api(&r, 0x0d) ? (short)r.x.ax : -1;
}

static int screen(int slot, unsigned char *cells, int *row, int *col, int *hidden)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  r.h.bl = (unsigned char)slot;
  r.x.es = __tb >> 4;
  r.x.di = __tb & 15;
  r.x.cx = COLS * ROWS * 2;
  if (!api(&r, 0x08))
    return 0;
  memset(cells, 0, COLS * ROWS * 2);
  dosmemget(__tb, r.x.si, cells);
  *row = r.h.ah;
  *col = r.h.al;
  *hidden = (r.x.bx & 0x2000) != 0;
  return 1;
}

static int type_keys(int slot, const unsigned short *keys, int n)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  dosmemput(keys, (size_t)n * 2, __tb);
  r.h.bl = (unsigned char)slot;
  r.x.ds = __tb >> 4;
  r.x.si = __tb & 15;
  r.x.cx = (unsigned short)n;
  return api(&r, 0x09) ? r.x.cx : 0;
}

/* ---------------------------------------------------------------- */

static void draw_cell(struct term *t, int i, int cursor)
{
  struct surface *s = t->win->client;
  unsigned char ch = t->shown[i * 2], a = t->shown[i * 2 + 1];
  char text[2] = { (char)(ch ? ch : ' '), 0 };
  int x = (i % COLS) * FONT_W, y = (i / COLS) * FONT_H;

  draw_text(s, x, y, text, cga[a & 15], cga[(a >> 4) & 7]);
  if (cursor)
    draw_fill(s, x, y + FONT_H - 3, FONT_W, 2, cga[a & 15]);
}

static void close_term(struct term *t, int end_job)
{
  int i;

  if (end_job)
    for (i = 0; i < 8; i++)
      if (!leaving[i])
      {
        leaving[i] = t->slot;           /* Ctrl+C and EXIT until it ends */
        break;
      }
  wm_window_close(t->win);
  t->win = 0;
  tlog("MDDESK: term closed\n");
  panel_damage();
}

static int events(struct window *w, const struct event *e)
{
  struct term *t = w->user;
  int i;

  if (e->type == EV_KEY)
  {
    unsigned short key = (unsigned short)(e->scan << 8 | (e->ascii & 0xff));
    type_keys(t->slot, &key, 1);
    return 1;
  }
  if (e->type == EV_REDRAW)
  {
    for (i = 0; i < COLS * ROWS; i++)
      draw_cell(t, i, 0);
    wm_invalidate(w);
    return 1;
  }
  if (e->type == EV_CLOSE)
  {
    close_term(t, 1);
    return 1;
  }
  return e->type == EV_MOUSE_DOWN;
}

/* called by the desktop when it is idle: screens, ended sessions */
void term_idle(void)
{
  static clock_t last_leave;
  clock_t now = clock();
  int i, n;

  if (now - last_poll < CLOCKS_PER_SEC / 20)
    return;
  last_poll = now;
  for (n = 0; n < MAX_TERMS; n++)
  {
    struct term *t = &terms[n];
    unsigned char cells[COLS * ROWS * 2];
    int row, col, hidden, changed = 0, cur, old;

    if (!t->win)
      continue;
    i = job_state(t->slot);
    if (i == 3 || i < 0)                /* the shell ended */
    {
      close_term(t, 0);
      continue;
    }
    if (!screen(t->slot, cells, &row, &col, &hidden))
      continue;
    cur = hidden || row >= ROWS || col >= COLS ? -1 : row * COLS + col;
    old = t->cursor_on ? t->row * COLS + t->col : -1;
    for (i = 0; i < COLS * ROWS; i++)
      if (!t->have || cells[i * 2] != t->shown[i * 2] || cells[i * 2 + 1] != t->shown[i * 2 + 1] ||
          (cur != old && (i == cur || i == old)))
      {
        t->shown[i * 2] = cells[i * 2];
        t->shown[i * 2 + 1] = cells[i * 2 + 1];
        draw_cell(t, i, i == cur);
        changed = 1;
      }
    t->have = 1;
    t->row = row;
    t->col = col;
    t->cursor_on = cur >= 0;
    if (changed)
    {
      wm_invalidate(t->win);
      if (test_mode)                    /* the cursor's line and the one above */
        for (i = row > 0 ? row - 1 : 0; i <= row && i < ROWS; i++)
        {
          char line[COLS + 1];
          int c;
          for (c = 0; c < COLS; c++)
            line[c] = (char)(t->shown[(i * COLS + c) * 2] >= ' ' ? t->shown[(i * COLS + c) * 2] : ' ');
          while (c > 0 && line[c - 1] == ' ')
            c--;
          line[c] = 0;
          if (c)
            tlog("MDDESK: term line %s\n", line);
          if (!t->ready && strchr(line, '>'))
          {
            t->ready = 1;
            tlog("MDDESK: term ready\n");
          }
        }
    }
  }
  /* sessions whose window was closed: ask the shell to leave */
  if (now - last_leave >= CLOCKS_PER_SEC)
  {
    static const unsigned short bye[] = { 0x2e03, 0x1c0d, 0x1265, 0x2d78, 0x1769, 0x1474, 0x1c0d };
    last_leave = now;
    for (i = 0; i < 8; i++)
      if (leaving[i])
      {
        int st = job_state(leaving[i]);
        if (st == 3 || st == 0 || st < 0)
          leaving[i] = 0;
        else
          type_keys(leaving[i], bye, 7);
      }
  }
}

static void message(const char *text)
{
  struct window *w = wm_window_new(200, 200, 560, 110, "Terminal");
  struct ui *ui;

  if (!w)
    return;
  ui = ui_new(w);
  ui_label(ui, 16, 16, text);
  tlog("MDDESK: term unavailable\n");
  wm_raise(w);
  panel_damage();
}

void term_open(void)
{
  struct term *t = 0;
  struct window *w;
  int i, slot;

  for (i = 0; i < MAX_TERMS; i++)
    if (!terms[i].win && !t)
      t = &terms[i];
  if (!uefi_boot())
  {
    message("The terminal needs a UEFI boot of modernDOS.");
    return;
  }
  if (!t || (slot = job_start()) < 0)
  {
    message("No free DOS session (start SSHSLOT before the desktop).");
    return;
  }
  w = wm_window_new(40 + 24 * (int)(t - terms), 50 + 24 * (int)(t - terms),
                    COLS * FONT_W + 2 * WM_BORDER, ROWS * FONT_H + WM_TITLE_H + 2 * WM_BORDER, "DOS");
  if (!w)
    return;
  memset(t, 0, sizeof(*t));
  t->win = w;
  t->slot = slot;
  w->user = t;
  w->handler = events;
  draw_fill(w->client, 0, 0, w->client->w, w->client->h, cga[0]);
  wm_raise(w);
  wm_invalidate(w);
  panel_damage();
  tlog("MDDESK: term opened in session %d, window %dx%d\n", slot, w->client->w, w->client->h);
}
