/* mdgui - window manager
 *
 * All windows live in one program (the desktop). Each window has a frame,
 * a title bar with a close button, and a retained client surface the
 * program draws into. The screen is composed from the desktop and the
 * windows, bottom to top, only where it was damaged, and then presented.
 *
 * Mouse: a press raises the window under the pointer and focuses it; on
 * the title bar it starts a move, on the close button it closes the
 * window; elsewhere it goes to the window (client coordinates), which
 * keeps the mouse until the button is released. Keys go to the focused
 * window.
 */
#include <stdlib.h>
#include <string.h>
#include "mdgui.h"

#define MAX_WINDOWS 32
#define CLOSE_W     18

/* theme */
#define C_DESK_TOP     RGB(36, 52, 71)
#define C_DESK_BOTTOM  RGB(18, 28, 40)
#define C_TITLE        RGB(58, 66, 82)
#define C_TITLE_ACTIVE RGB(32, 128, 136)
#define C_TITLE_TEXT   RGB(236, 240, 244)
#define C_BORDER       RGB(20, 24, 30)
#define C_CLOSE        RGB(196, 72, 64)

static struct window *wins[MAX_WINDOWS];  /* bottom to top */
static int nwin;
static struct rect dmg;
static int dmg_set, quit;
static struct window *drag, *capture;
static int drag_dx, drag_dy;
/* the desktop's own content (a panel): drawn over the background, gets
   the mouse where no window is; windows stay below desk_top */
static int desk_top;
static void (*desk_draw)(struct surface *s);
static int (*desk_event)(const struct event *e);

void wm_desktop(int top, void (*draw)(struct surface *s), int (*event)(const struct event *e))
{
  struct surface *s = gfx_screen();

  desk_top = top;
  desk_draw = draw;
  desk_event = event;
  if (s)
    wm_damage(0, 0, s->w, s->h);
}

int wm_desktop_top(void)
{
  return desk_top;
}

static int clamp_y(const struct window *w, int y)
{
  return !w->popup && y < desk_top ? desk_top : y;
}

int wm_count(void)
{
  return nwin;
}

struct window *wm_window(int i)
{
  return i >= 0 && i < nwin ? wins[i] : 0;
}

struct window *wm_top(void)
{
  int i;
  for (i = nwin - 1; i >= 0; i--)
    if (wins[i]->visible)
      return wins[i];
  return 0;
}

struct window *wm_focus(void)
{
  return wm_top();
}

void wm_damage(int x, int y, int w, int h)
{
  int x1, y1;

  if (w <= 0 || h <= 0)
    return;
  if (!dmg_set)
  {
    dmg.x = x;
    dmg.y = y;
    dmg.w = w;
    dmg.h = h;
    dmg_set = 1;
    return;
  }
  x1 = dmg.x + dmg.w > x + w ? dmg.x + dmg.w : x + w;
  y1 = dmg.y + dmg.h > y + h ? dmg.y + dmg.h : y + h;
  if (x < dmg.x) dmg.x = x;
  if (y < dmg.y) dmg.y = y;
  dmg.w = x1 - dmg.x;
  dmg.h = y1 - dmg.y;
}

static void damage_window(struct window *w)
{
  wm_damage(w->x, w->y, w->w, w->h);
}

void wm_invalidate(struct window *w)
{
  damage_window(w);
}

static void client_rect(const struct window *w, struct rect *r)
{
  int top = w->popup ? WM_BORDER : WM_TITLE_H;

  r->x = w->x + WM_BORDER;
  r->y = w->y + top;
  r->w = w->w - 2 * WM_BORDER;
  r->h = w->h - top - WM_BORDER;
}

static struct window *window_new(int x, int y, int w, int h, const char *title, int popup)
{
  struct window *win;
  struct rect c;

  if (nwin == MAX_WINDOWS || w < 16 || h < (popup ? 8 : WM_TITLE_H + 16))
    return 0;
  win = calloc(1, sizeof(*win));
  if (!win)
    return 0;
  win->popup = popup;
  if (!popup && y < desk_top)
    y = desk_top;
  win->x = x;
  win->y = y;
  win->w = w;
  win->h = h;
  strncpy(win->title, title, sizeof(win->title) - 1);
  client_rect(win, &c);
  win->client = surface_new(c.w, c.h);
  if (!win->client)
  {
    free(win);
    return 0;
  }
  draw_fill(win->client, 0, 0, c.w, c.h, RGB(242, 244, 247));
  win->visible = 1;
  wins[nwin++] = win;
  damage_window(win);
  if (nwin > 1)
    damage_window(wins[nwin - 2]);      /* it loses the focus look */
  return win;
}

struct window *wm_window_new(int x, int y, int w, int h, const char *title)
{
  return window_new(x, y, w, h, title, 0);
}

/* a popup (menu): frame and client only, on top; a mouse press outside
   it closes it (the handler gets EV_CLOSE first) */
struct window *wm_popup_new(int x, int y, int w, int h)
{
  return window_new(x, y, w, h, "", 1);
}

void wm_window_close(struct window *w)
{
  int i, j;

  for (i = 0; i < nwin && wins[i] != w; i++)
    ;
  if (i == nwin)
    return;
  damage_window(w);
  for (j = i; j < nwin - 1; j++)
    wins[j] = wins[j + 1];
  nwin--;
  if (drag == w)
    drag = 0;
  if (capture == w)
    capture = 0;
  if (wm_top())
    damage_window(wm_top());            /* gets the focus look */
  surface_free(w->client);
  free(w);
}

void wm_window_move(struct window *w, int x, int y)
{
  damage_window(w);
  w->x = x;
  w->y = clamp_y(w, y);
  damage_window(w);
}

void wm_raise(struct window *w)
{
  int i, j;

  for (i = 0; i < nwin && wins[i] != w; i++)
    ;
  if (i == nwin || i == nwin - 1)
    return;
  damage_window(wins[nwin - 1]);
  for (j = i; j < nwin - 1; j++)
    wins[j] = wins[j + 1];
  wins[nwin - 1] = w;
  damage_window(w);
}

/* ---------------------------------------------------------------- */

static void draw_desktop(struct surface *s)
{
  draw_gradient(s, 0, 0, s->w, s->h, C_DESK_TOP, C_DESK_BOTTOM);
  draw_text(s, s->w - text_width("modernDOS") - 16, s->h - FONT_H - 12, "modernDOS",
            RGB(90, 110, 130), TEXT_TRANSPARENT);
  if (desk_draw)
    desk_draw(s);
}

static void close_box(const struct window *w, struct rect *r)
{
  r->w = CLOSE_W;
  r->h = CLOSE_W - 2;
  r->x = w->x + w->w - CLOSE_W - 4;
  r->y = w->y + (WM_TITLE_H - r->h) / 2;
}

static void draw_window(struct surface *s, const struct window *w, int active)
{
  struct rect c, cb;
  color_t title = active ? C_TITLE_ACTIVE : C_TITLE;
  int tw = w->w - CLOSE_W - 20, i;
  char t[64];

  draw_frame(s, w->x, w->y, w->w, w->h, w->popup ? RGB(120, 128, 140) : C_BORDER);
  if (w->popup)
  {
    client_rect(w, &c);
    draw_blit(s, c.x, c.y, w->client, 0, 0, c.w, c.h);
    return;
  }
  draw_fill(s, w->x + 1, w->y + 1, w->w - 2, WM_TITLE_H - 1, title);
  draw_hline(s, w->x + 1, w->y + 1, w->w - 2, active ? RGB(64, 168, 176) : RGB(78, 86, 102));
  /* title text, cut to fit */
  strcpy(t, w->title);
  for (i = (int)strlen(t); i > 0 && text_width(t) > tw; i--)
    t[i - 1] = 0;
  draw_text(s, w->x + 10, w->y + (WM_TITLE_H - FONT_H) / 2, t, C_TITLE_TEXT, TEXT_TRANSPARENT);
  close_box(w, &cb);
  draw_fill(s, cb.x, cb.y, cb.w, cb.h, active ? C_CLOSE : RGB(88, 96, 112));
  draw_line(s, cb.x + 5, cb.y + 4, cb.x + cb.w - 6, cb.y + cb.h - 5, C_TITLE_TEXT);
  draw_line(s, cb.x + cb.w - 6, cb.y + 4, cb.x + 5, cb.y + cb.h - 5, C_TITLE_TEXT);
  client_rect(w, &c);
  draw_blit(s, c.x, c.y, w->client, 0, 0, c.w, c.h);
}

void wm_update(void)
{
  struct surface *s = gfx_screen();
  struct window *top = 0;
  int i;

  for (i = nwin - 1; i >= 0 && !top; i--)
    if (wins[i]->visible && !wins[i]->popup)
      top = wins[i];

  if (!dmg_set || !s)
    return;
  surface_clip(s, dmg.x, dmg.y, dmg.w, dmg.h);
  draw_desktop(s);
  for (i = 0; i < nwin; i++)
  {
    const struct window *w = wins[i];
    if (w->visible && w->x < dmg.x + dmg.w && w->x + w->w > dmg.x &&
        w->y < dmg.y + dmg.h && w->y + w->h > dmg.y)
      draw_window(s, w, w == top);
  }
  surface_noclip(s);
  gfx_present(dmg.x, dmg.y, dmg.w, dmg.h);
  dmg_set = 0;
}

int wm_init(void)
{
  struct surface *s = gfx_screen();

  if (!s)
    return -1;
  nwin = 0;
  drag = capture = 0;
  quit = 0;
  wm_damage(0, 0, s->w, s->h);
  wm_update();
  return 0;
}

/* ---------------------------------------------------------------- */

static struct window *window_at(int x, int y)
{
  int i;
  for (i = nwin - 1; i >= 0; i--)
  {
    struct window *w = wins[i];
    if (w->visible && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
      return w;
  }
  return 0;
}

static int deliver(struct window *w, const struct event *e)
{
  struct event c = *e;
  struct rect r;

  if (!w || !w->handler)
    return 0;
  client_rect(w, &r);
  c.x -= r.x;
  c.y -= r.y;
  return w->handler(w, &c);
}

int wm_dispatch(const struct event *e)
{
  struct window *w;
  struct rect cb;

  if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_UP)
    gfx_cursor_move(e->x, e->y);
  switch (e->type)
  {
    case EV_MOUSE_DOWN:
      w = window_at(e->x, e->y);
      if (wm_top() && wm_top()->popup && w != wm_top())
      {                                 /* outside a popup: close it */
        struct window *p = wm_top();
        struct event c = *e;
        c.type = EV_CLOSE;
        if (!deliver(p, &c))
          wm_window_close(p);
        return 1;
      }
      if (!w)
        return desk_event ? desk_event(e) : 0;
      if (w->popup)
      {
        capture = w;
        return deliver(w, e);
      }
      wm_raise(w);
      if (e->button != MB_LEFT || e->y >= w->y + WM_TITLE_H)
      {
        capture = w;
        return deliver(w, e);
      }
      close_box(w, &cb);
      if (e->x >= cb.x && e->x < cb.x + cb.w && e->y >= cb.y && e->y < cb.y + cb.h)
      {
        struct event c = *e;
        c.type = EV_CLOSE;
        if (!deliver(w, &c))
          wm_window_close(w);
        return 1;
      }
      drag = w;                         /* title bar: move */
      drag_dx = e->x - w->x;
      drag_dy = e->y - w->y;
      return 1;
    case EV_MOUSE_MOVE:
      if (drag)
      {
        wm_window_move(drag, e->x - drag_dx, e->y - drag_dy);
        return 1;
      }
      w = capture ? capture : window_at(e->x, e->y);
      if (!w)
        return desk_event ? desk_event(e) : 0;
      return deliver(w, e);
    case EV_MOUSE_UP:
      if (drag)
      {
        drag = 0;
        return 1;
      }
      w = capture ? capture : window_at(e->x, e->y);
      if (!e->buttons)
        capture = 0;
      if (!w)
        return desk_event ? desk_event(e) : 0;
      return deliver(w, e);
    case EV_KEY:
      if (!wm_focus())
        return desk_event ? desk_event(e) : 0;
      return deliver(wm_focus(), e);
  }
  return 0;
}

void wm_quit(void)
{
  quit = 1;
}

void wm_run(int (*idle)(void))
{
  struct event e;

  quit = 0;
  while (!quit)
  {
    wm_update();
    if (idle)
    {
      if (!input_poll(&e))
      {
        if (!idle())
          input_wait(&e);
        else
          continue;
      }
    }
    else
      input_wait(&e);
    wm_dispatch(&e);
  }
  wm_update();
}
