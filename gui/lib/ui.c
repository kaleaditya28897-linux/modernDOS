/* mdgui - widget toolkit
 *
 * Widgets live in a window's client area and are drawn into its client
 * surface. ui_new() installs a handler on the window; events no widget
 * uses (and EV_CLOSE) go on to the window's previous handler.
 *
 * Keyboard: Tab / Shift+Tab move the focus between focusable widgets;
 * buttons take Enter/Space, checkboxes Space, lists Up/Down/Home/End and
 * Enter, text fields editing keys and Enter (action). Menus open with the
 * mouse on the menu bar, or F10; in a menu Up/Down/Enter/Esc work.
 */
#include <stdlib.h>
#include <string.h>
#include "mdgui.h"

#define MAX_WIDGETS 64
#define ROW_H       20
#define MENUBAR_H   22
#define SCROLL_W    12

/* theme */
#define C_FACE      RGB(242, 244, 247)
#define C_INK       RGB(30, 34, 40)
#define C_INK_DIM   RGB(140, 146, 156)
#define C_LINE      RGB(150, 158, 170)
#define C_BUTTON    RGB(224, 228, 234)
#define C_PRESSED   RGB(196, 204, 214)
#define C_FIELD     RGB(255, 255, 255)
#define C_ACCENT    RGB(32, 128, 136)
#define C_ACCENT_TX RGB(255, 255, 255)
#define C_BAR       RGB(228, 232, 238)

struct ui {
  struct window *win;
  wm_handler next;
  struct widget *w[MAX_WIDGETS];
  int n;
  struct widget *focus, *pressed;
  /* open menu */
  struct window *popup;
  struct widget *menubar;
  int open;
};

static int focusable(const struct widget *w)
{
  return !w->disabled && w->type != UI_LABEL && w->type != UI_MENUBAR;
}

static struct widget *add(struct ui *ui, int type, int x, int y, int w, int h, const char *text)
{
  struct widget *wd;

  if (ui->n == MAX_WIDGETS || !(wd = calloc(1, sizeof(*wd))))
    return 0;
  wd->type = type;
  wd->x = x;
  wd->y = y;
  wd->w = w;
  wd->h = h;
  if (text)
    strncpy(wd->text, text, sizeof(wd->text) - 1);
  wd->ui = ui;
  ui->w[ui->n++] = wd;
  if (!ui->focus && focusable(wd))
    ui->focus = wd;
  return wd;
}

struct widget *ui_label(struct ui *ui, int x, int y, const char *text)
{
  return add(ui, UI_LABEL, x, y, text_width(text), FONT_H, text);
}

struct widget *ui_button(struct ui *ui, int x, int y, int w, int h, const char *text, ui_action a)
{
  struct widget *wd = add(ui, UI_BUTTON, x, y, w, h, text);
  if (wd)
    wd->action = a;
  return wd;
}

struct widget *ui_check(struct ui *ui, int x, int y, const char *text, ui_action a)
{
  struct widget *wd = add(ui, UI_CHECK, x, y, 22 + text_width(text), 18, text);
  if (wd)
    wd->action = a;
  return wd;
}

struct widget *ui_textfield(struct ui *ui, int x, int y, int w, ui_action a)
{
  struct widget *wd = add(ui, UI_TEXT, x, y, w, FONT_H + 8, "");
  if (wd)
    wd->action = a;
  return wd;
}

struct widget *ui_list(struct ui *ui, int x, int y, int w, int h, const char *const *items, int n,
                       ui_action a)
{
  struct widget *wd = add(ui, UI_LIST, x, y, w, h, 0);
  if (wd)
  {
    wd->items = items;
    wd->nitems = n;
    wd->value = -1;
    wd->action = a;
  }
  return wd;
}

struct widget *ui_menubar(struct ui *ui, const struct ui_menu *menus, int n, ui_action a)
{
  struct widget *wd = add(ui, UI_MENUBAR, 0, 0, ui->win->client->w, MENUBAR_H, 0);
  if (wd)
  {
    wd->menus = menus;
    wd->nmenus = n;
    wd->action = a;
    ui->menubar = wd;
  }
  return wd;
}

void ui_set_text(struct widget *w, const char *text)
{
  strncpy(w->text, text, sizeof(w->text) - 1);
  w->text[sizeof(w->text) - 1] = 0;
  if (w->type == UI_TEXT)
  {
    w->value = (int)strlen(w->text);
    w->scroll = 0;
  }
  ui_redraw(w->ui);
}

struct widget *ui_focused(struct ui *ui)
{
  return ui->focus;
}

void ui_focus(struct widget *w)
{
  if (w && focusable(w))
  {
    w->ui->focus = w;
    ui_redraw(w->ui);
  }
}

int ui_widget_center(const struct widget *w, int *sx, int *sy)
{
  const struct window *win = w->ui->win;
  int top = win->popup ? WM_BORDER : WM_TITLE_H;

  *sx = win->x + WM_BORDER + w->x + w->w / 2;
  *sy = win->y + top + w->y + w->h / 2;
  return 0;
}

/* ---------------------------------------------------------------- */
/* drawing                                                          */

static int list_rows(const struct widget *w)
{
  return (w->h - 4) / ROW_H;
}

static int menu_title_x(const struct widget *w, int i)
{
  int x = 8, j;
  for (j = 0; j < i; j++)
    x += text_width(w->menus[j].title) + 20;
  return x;
}

static void draw_widget(struct surface *s, struct widget *w, int focused)
{
  int i, tx, ty;
  color_t frame = focused ? C_ACCENT : C_LINE;

  switch (w->type)
  {
    case UI_LABEL:
      draw_text(s, w->x, w->y, w->text, w->disabled ? C_INK_DIM : C_INK, TEXT_TRANSPARENT);
      break;
    case UI_BUTTON:
      draw_fill(s, w->x, w->y, w->w, w->h, w->ui->pressed == w ? C_PRESSED : C_BUTTON);
      draw_frame(s, w->x, w->y, w->w, w->h, frame);
      if (focused)
        draw_frame(s, w->x + 1, w->y + 1, w->w - 2, w->h - 2, frame);
      tx = w->x + (w->w - text_width(w->text)) / 2;
      ty = w->y + (w->h - FONT_H) / 2;
      draw_text(s, tx, ty, w->text, w->disabled ? C_INK_DIM : C_INK, TEXT_TRANSPARENT);
      break;
    case UI_CHECK:
      draw_fill(s, w->x, w->y + 2, 14, 14, w->value ? C_ACCENT : C_FIELD);
      draw_frame(s, w->x, w->y + 2, 14, 14, w->value ? C_ACCENT : frame);
      if (w->value)
      {
        draw_line(s, w->x + 3, w->y + 9, w->x + 6, w->y + 12, C_ACCENT_TX);
        draw_line(s, w->x + 6, w->y + 12, w->x + 11, w->y + 5, C_ACCENT_TX);
        draw_line(s, w->x + 3, w->y + 8, w->x + 6, w->y + 11, C_ACCENT_TX);
        draw_line(s, w->x + 6, w->y + 11, w->x + 11, w->y + 4, C_ACCENT_TX);
      }
      draw_text(s, w->x + 22, w->y + 1, w->text, C_INK, TEXT_TRANSPARENT);
      if (focused)
        draw_hline(s, w->x + 22, w->y + 17, text_width(w->text), C_ACCENT);
      break;
    case UI_TEXT:
    {
      int visible = (w->w - 10) / FONT_W, len = (int)strlen(w->text);
      char part[128];
      if (w->value < w->scroll)
        w->scroll = w->value;
      if (w->value > w->scroll + visible)
        w->scroll = w->value - visible;
      draw_fill(s, w->x, w->y, w->w, w->h, C_FIELD);
      draw_frame(s, w->x, w->y, w->w, w->h, frame);
      strncpy(part, w->text + (w->scroll < len ? w->scroll : len), sizeof(part) - 1);
      part[visible < 127 ? visible : 127] = 0;
      draw_text(s, w->x + 5, w->y + 4, part, C_INK, TEXT_TRANSPARENT);
      if (focused)
        draw_vline(s, w->x + 5 + (w->value - w->scroll) * FONT_W, w->y + 3, FONT_H + 2, C_INK);
      break;
    }
    case UI_LIST:
    {
      int rows = list_rows(w), iw = w->nitems > rows ? w->w - SCROLL_W : w->w;
      draw_fill(s, w->x, w->y, w->w, w->h, C_FIELD);
      draw_frame(s, w->x, w->y, w->w, w->h, frame);
      surface_clip(s, w->x + 1, w->y + 1, iw - 2, w->h - 2);
      for (i = 0; i < rows && w->scroll + i < w->nitems; i++)
      {
        int item = w->scroll + i, y = w->y + 2 + i * ROW_H;
        int sel = item == w->value;
        if (sel)
          draw_fill(s, w->x + 2, y, iw - 4, ROW_H, focused ? C_ACCENT : C_PRESSED);
        draw_text(s, w->x + 6, y + (ROW_H - FONT_H) / 2, w->items[item],
                  sel && focused ? C_ACCENT_TX : C_INK, TEXT_TRANSPARENT);
      }
      surface_noclip(s);
      if (w->nitems > rows)             /* scroll bar */
      {
        int track = w->h - 2, th = track * rows / w->nitems;
        int ty0 = w->y + 1 + (track - th) * w->scroll / (w->nitems - rows);
        draw_fill(s, w->x + w->w - SCROLL_W - 1, w->y + 1, SCROLL_W, track, RGB(232, 235, 240));
        draw_fill(s, w->x + w->w - SCROLL_W + 1, ty0 + 1, SCROLL_W - 4, th - 2, RGB(176, 184, 196));
      }
      break;
    }
    case UI_MENUBAR:
      draw_fill(s, 0, 0, s->w, MENUBAR_H, C_BAR);
      draw_hline(s, 0, MENUBAR_H - 1, s->w, RGB(206, 212, 220));
      for (i = 0; i < w->nmenus; i++)
      {
        int x = menu_title_x(w, i), open = w->ui->popup && w->ui->open == i;
        if (open)
          draw_fill(s, x - 6, 0, text_width(w->menus[i].title) + 12, MENUBAR_H - 1, C_ACCENT);
        draw_text(s, x, (MENUBAR_H - FONT_H) / 2, w->menus[i].title,
                  open ? C_ACCENT_TX : C_INK, TEXT_TRANSPARENT);
      }
      break;
  }
}

void ui_redraw(struct ui *ui)
{
  struct surface *s = ui->win->client;
  int i;

  draw_fill(s, 0, 0, s->w, s->h, C_FACE);
  for (i = 0; i < ui->n; i++)
    draw_widget(s, ui->w[i], ui->w[i] == ui->focus && !ui->popup);
  wm_invalidate(ui->win);
}

/* ---------------------------------------------------------------- */
/* popup menus                                                      */

/* one popup menu at a time */
static struct {
  struct window *win;
  const char *const *items;
  int n, hot;
  void (*choose)(int item, void *user);
  void *user;
} pm;

static void pm_draw(void)
{
  struct surface *s = pm.win->client;
  int i;

  draw_fill(s, 0, 0, s->w, s->h, C_FIELD);
  for (i = 0; i < pm.n; i++)
  {
    if (i == pm.hot)
      draw_fill(s, 2, 2 + i * ROW_H, s->w - 4, ROW_H, C_ACCENT);
    draw_text(s, 12, 2 + i * ROW_H + (ROW_H - FONT_H) / 2, pm.items[i],
              i == pm.hot ? C_ACCENT_TX : C_INK, TEXT_TRANSPARENT);
  }
  wm_invalidate(pm.win);
}

/* close the popup; tell the owner the item (-1: none) */
static void pm_end(int item)
{
  void (*choose)(int, void *) = pm.choose;
  void *user = pm.user;
  struct window *w = pm.win;

  if (!w)
    return;
  pm.win = 0;
  wm_window_close(w);
  if (choose)
    choose(item, user);
}

static int pm_handler(struct window *win, const struct event *e)
{
  int row;

  switch (e->type)
  {
    case EV_CLOSE:
      pm_end(-1);
      return 1;
    case EV_MOUSE_MOVE:
    case EV_MOUSE_DOWN:
      row = e->y >= 2 ? (e->y - 2) / ROW_H : -1;
      if (row >= 0 && row < pm.n && e->x >= 0 && e->x < win->client->w && row != pm.hot)
      {
        pm.hot = row;
        pm_draw();
      }
      return 1;
    case EV_MOUSE_UP:
      row = e->y >= 2 ? (e->y - 2) / ROW_H : -1;
      if (row >= 0 && row < pm.n && e->x >= 0 && e->x < win->client->w)
        pm_end(row);
      return 1;
    case EV_KEY:
      if (e->scan == 0x48 && pm.hot > 0)                  /* up */
        pm.hot--;
      else if (e->scan == 0x50 && pm.hot < pm.n - 1)      /* down */
        pm.hot++;
      else if (e->ascii == 13)
      {
        pm_end(pm.hot);
        return 1;
      }
      else if (e->ascii == 27)
      {
        pm_end(-1);
        return 1;
      }
      pm_draw();
      return 1;
  }
  return 0;
}

int ui_popup(int x, int y, const char *const *items, int n, void (*choose)(int item, void *user),
             void *user)
{
  int w = 0, j;

  pm_end(-1);
  for (j = 0; j < n; j++)
    if (text_width(items[j]) > w)
      w = text_width(items[j]);
  pm.items = items;
  pm.n = n;
  pm.hot = 0;
  pm.choose = choose;
  pm.user = user;
  pm.win = wm_popup_new(x, y, w + 40, n * ROW_H + 6);
  if (!pm.win)
    return -1;
  pm.win->handler = pm_handler;
  pm_draw();
  return 0;
}

int ui_popup_open(void)
{
  return pm.win != 0;
}

/* the menu bar's menus */
static void bar_choose(int item, void *user)
{
  struct ui *ui = user;
  struct widget *bar = ui->menubar;
  int menu = ui->open;

  ui->popup = 0;
  ui_redraw(ui);
  if (item >= 0 && bar->action)
  {
    bar->menu = menu;
    bar->value = item;
    bar->action(bar);
  }
}

static void menu_open(struct ui *ui, int i)
{
  const struct ui_menu *m = &ui->menubar->menus[i];

  ui->open = i;
  ui->popup = 0;
  if (ui_popup(ui->win->x + WM_BORDER + menu_title_x(ui->menubar, i) - 6,
               ui->win->y + WM_TITLE_H + MENUBAR_H - 1, m->items, m->nitems, bar_choose, ui))
    return;
  ui->popup = pm.win;
  ui_redraw(ui);
}

/* ---------------------------------------------------------------- */
/* events                                                           */

static struct widget *widget_at(struct ui *ui, int x, int y)
{
  int i;
  for (i = ui->n - 1; i >= 0; i--)
  {
    struct widget *w = ui->w[i];
    if (!w->disabled && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
      return w;
  }
  return 0;
}

static void act(struct widget *w)
{
  if (w->action)
    w->action(w);
}

static void list_select(struct widget *w, int item)
{
  int rows = list_rows(w);

  if (item < 0)
    item = 0;
  if (item >= w->nitems)
    item = w->nitems - 1;
  w->value = item;
  if (item < w->scroll)
    w->scroll = item;
  if (item >= w->scroll + rows)
    w->scroll = item - rows + 1;
}

static void focus_next(struct ui *ui, int back)
{
  int i, start = 0, k;

  for (i = 0; i < ui->n; i++)
    if (ui->w[i] == ui->focus)
      start = i;
  for (k = 1; k <= ui->n; k++)
  {
    struct widget *w = ui->w[((back ? start - k : start + k) % ui->n + ui->n) % ui->n];
    if (focusable(w))
    {
      ui->focus = w;
      return;
    }
  }
}

static int text_key(struct widget *w, const struct event *e)
{
  int len = (int)strlen(w->text);

  switch (e->scan)
  {
    case 0x4b: if (w->value > 0) w->value--; return 1;           /* left */
    case 0x4d: if (w->value < len) w->value++; return 1;         /* right */
    case 0x47: w->value = 0; return 1;                           /* home */
    case 0x4f: w->value = len; return 1;                         /* end */
    case 0x53:                                                   /* delete */
      if (w->value < len)
        memmove(w->text + w->value, w->text + w->value + 1, len - w->value);
      return 1;
  }
  if (e->ascii == 8)
  {
    if (w->value > 0)
    {
      memmove(w->text + w->value - 1, w->text + w->value, len - w->value + 1);
      w->value--;
    }
    return 1;
  }
  if (e->ascii == 13)
  {
    act(w);
    return 1;
  }
  if (e->ascii >= 32 && e->ascii < 256 && e->ascii != 127 && len < (int)sizeof(w->text) - 1)
  {
    memmove(w->text + w->value + 1, w->text + w->value, len - w->value + 1);
    w->text[w->value++] = (char)e->ascii;
    return 1;
  }
  return 0;
}

static int key(struct ui *ui, const struct event *e)
{
  struct widget *w = ui->focus;

  if (e->ascii == 9 || (e->scan == 0x0f && !e->ascii))  /* Tab, Shift+Tab */
  {
    focus_next(ui, e->ascii != 9 || (e->shift & 3));
    return 1;
  }
  if (e->scan == 0x44 && ui->menubar)                   /* F10: first menu */
  {
    menu_open(ui, 0);
    return 1;
  }
  if (!w)
    return 0;
  switch (w->type)
  {
    case UI_BUTTON:
      if (e->ascii == 13 || e->ascii == ' ')
      {
        act(w);
        return 1;
      }
      break;
    case UI_CHECK:
      if (e->ascii == ' ')
      {
        w->value = !w->value;
        act(w);
        return 1;
      }
      break;
    case UI_LIST:
      if (e->scan == 0x48) { list_select(w, w->value - 1); act(w); return 1; }
      if (e->scan == 0x50) { list_select(w, w->value + 1); act(w); return 1; }
      if (e->scan == 0x47) { list_select(w, 0); act(w); return 1; }
      if (e->scan == 0x4f) { list_select(w, w->nitems - 1); act(w); return 1; }
      if (e->ascii == 13) { act(w); return 1; }
      break;
    case UI_TEXT:
      return text_key(w, e);
  }
  return 0;
}

static int mouse_down(struct ui *ui, const struct event *e)
{
  struct widget *w = widget_at(ui, e->x, e->y);
  int i;

  if (!w)
    return 0;
  if (w->type == UI_MENUBAR)
  {
    for (i = w->nmenus - 1; i >= 0; i--)
      if (e->x >= menu_title_x(w, i) - 6)
      {
        if (e->x < menu_title_x(w, i) + text_width(w->menus[i].title) + 6)
          menu_open(ui, i);
        break;
      }
    return 1;
  }
  if (focusable(w))
    ui->focus = w;
  switch (w->type)
  {
    case UI_BUTTON:
      ui->pressed = w;
      break;
    case UI_CHECK:
      w->value = !w->value;
      act(w);
      break;
    case UI_TEXT:
    {
      int pos = w->scroll + (e->x - w->x - 5 + FONT_W / 2) / FONT_W, len = (int)strlen(w->text);
      w->value = pos < 0 ? 0 : pos > len ? len : pos;
      break;
    }
    case UI_LIST:
    {
      int rows = list_rows(w);
      if (w->nitems > rows && e->x >= w->x + w->w - SCROLL_W - 1)
      {                                 /* scroll bar: page up/down */
        w->scroll += e->y < w->y + w->h / 2 ? -rows : rows;
        if (w->scroll > w->nitems - rows)
          w->scroll = w->nitems - rows;
        if (w->scroll < 0)
          w->scroll = 0;
        break;
      }
      i = w->scroll + (e->y - w->y - 2) / ROW_H;
      if (e->y >= w->y + 2 && i < w->nitems)
      {
        list_select(w, i);
        act(w);
      }
      break;
    }
  }
  return 1;
}

static int handler(struct window *win, const struct event *e)
{
  struct ui *ui = win->user;
  int used = 0;

  switch (e->type)
  {
    case EV_MOUSE_DOWN:
      if (e->button == MB_LEFT)
        used = mouse_down(ui, e);
      break;
    case EV_MOUSE_UP:
      if (ui->pressed)
      {
        struct widget *w = ui->pressed;
        ui->pressed = 0;
        if (widget_at(ui, e->x, e->y) == w)
          act(w);
        used = 1;
      }
      break;
    case EV_KEY:
      used = key(ui, e);
      break;
  }
  if (used)
  {
    if (ui->win == win)                 /* the action may have closed it */
      ui_redraw(ui);
    return 1;
  }
  return ui->next ? ui->next(win, e) : 0;
}

struct ui *ui_new(struct window *win)
{
  struct ui *ui = calloc(1, sizeof(*ui));

  if (!ui)
    return 0;
  ui->win = win;
  ui->next = win->handler;
  win->handler = handler;
  win->user = ui;
  ui_redraw(ui);
  return ui;
}
