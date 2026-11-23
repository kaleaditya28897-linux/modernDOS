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
#include <time.h>
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
/* multi-line text (UI_EDIT)                                        */

#define TAB_SPACES 4

struct ebuf {
  char **line;
  int *len;
  int n, cap;
  int cl, cc;                           /* caret */
  int top, left;                        /* first visible line, column */
  int modified;
};

static int eb_grow(struct ebuf *b, int n)
{
  if (n <= b->cap)
    return 0;
  {
    int cap = b->cap ? b->cap * 2 : 64;
    char **l;
    int *ln;
    while (cap < n)
      cap *= 2;
    l = realloc(b->line, cap * sizeof(char *));
    if (!l)
      return -1;
    b->line = l;
    ln = realloc(b->len, cap * sizeof(int));
    if (!ln)
      return -1;
    b->len = ln;
    b->cap = cap;
  }
  return 0;
}

static void eb_clear(struct ebuf *b)
{
  int i;
  for (i = 0; i < b->n; i++)
    free(b->line[i]);
  b->n = 0;
}

/* insert a line (copy of s, len) at i */
static int eb_insert_line(struct ebuf *b, int i, const char *s, int len)
{
  char *l;

  if (eb_grow(b, b->n + 1) || !(l = malloc(len + 1)))
    return -1;
  memcpy(l, s, len);
  l[len] = 0;
  memmove(b->line + i + 1, b->line + i, (b->n - i) * sizeof(char *));
  memmove(b->len + i + 1, b->len + i, (b->n - i) * sizeof(int));
  b->line[i] = l;
  b->len[i] = len;
  b->n++;
  return 0;
}

static void eb_delete_line(struct ebuf *b, int i)
{
  free(b->line[i]);
  memmove(b->line + i, b->line + i + 1, (b->n - i - 1) * sizeof(char *));
  memmove(b->len + i, b->len + i + 1, (b->n - i - 1) * sizeof(int));
  b->n--;
}

/* replace line i's text */
static int eb_set_line(struct ebuf *b, int i, const char *s, int len)
{
  char *l = malloc(len + 1);
  if (!l)
    return -1;
  memcpy(l, s, len);
  l[len] = 0;
  free(b->line[i]);
  b->line[i] = l;
  b->len[i] = len;
  return 0;
}

struct widget *ui_edit(struct ui *ui, int x, int y, int w, int h, ui_action a)
{
  struct widget *wd = add(ui, UI_EDIT, x, y, w, h, 0);
  struct ebuf *b;

  if (!wd)
    return 0;
  b = calloc(1, sizeof(*b));
  if (!b || eb_insert_line(b, 0, "", 0))
    return 0;
  wd->data = b;
  wd->action = a;
  return wd;
}

int edit_set_text(struct widget *w, const char *text, long len)
{
  struct ebuf *b = w->data;
  long i, start = 0;

  eb_clear(b);
  for (i = 0; i <= len; i++)
    if (i == len || text[i] == '\n')
    {
      long end = i;
      if (end > start && text[end - 1] == '\r')
        end--;
      if (i == len && start == len && b->n)
        break;                          /* text ends with a newline */
      if (eb_insert_line(b, b->n, text + start, (int)(end - start)))
        return -1;
      start = i + 1;
    }
  if (!b->n && eb_insert_line(b, 0, "", 0))
    return -1;
  b->cl = b->cc = b->top = b->left = 0;
  b->modified = 0;
  ui_redraw(w->ui);
  return 0;
}

char *edit_get_text(struct widget *w, long *len)
{
  struct ebuf *b = w->data;
  long total = 0;
  char *t, *p;
  int i;

  for (i = 0; i < b->n; i++)
    total += b->len[i] + 2;
  t = malloc(total + 1);
  if (!t)
    return 0;
  for (p = t, i = 0; i < b->n; i++)
  {
    memcpy(p, b->line[i], b->len[i]);
    p += b->len[i];
    *p++ = '\r';
    *p++ = '\n';
  }
  *p = 0;
  if (len)
    *len = total;
  return t;
}

int edit_modified(struct widget *w)
{
  return ((struct ebuf *)w->data)->modified;
}

void edit_set_modified(struct widget *w, int m)
{
  ((struct ebuf *)w->data)->modified = m;
}

void edit_caret(struct widget *w, int *line, int *col)
{
  struct ebuf *b = w->data;
  *line = b->cl + 1;
  *col = b->cc + 1;
}

int edit_lines(struct widget *w)
{
  return ((struct ebuf *)w->data)->n;
}

static int edit_rows(const struct widget *w)
{
  return (w->h - 6) / FONT_H;
}

static int edit_cols(const struct widget *w)
{
  return (w->w - 10 - SCROLL_W) / FONT_W;
}

/* keep the caret in the text and on the screen */
static void edit_fix(struct widget *w)
{
  struct ebuf *b = w->data;
  int rows = edit_rows(w), cols = edit_cols(w);

  if (b->cl < 0) b->cl = 0;
  if (b->cl >= b->n) b->cl = b->n - 1;
  if (b->cc < 0) b->cc = 0;
  if (b->cc > b->len[b->cl]) b->cc = b->len[b->cl];
  if (b->cl < b->top) b->top = b->cl;
  if (b->cl >= b->top + rows) b->top = b->cl - rows + 1;
  if (b->cc < b->left) b->left = b->cc;
  if (b->cc >= b->left + cols) b->left = b->cc - cols + 1;
}

static void edit_insert(struct ebuf *b, const char *s, int n)
{
  char *l = malloc(b->len[b->cl] + n + 1);

  if (!l)
    return;
  memcpy(l, b->line[b->cl], b->cc);
  memcpy(l + b->cc, s, n);
  memcpy(l + b->cc + n, b->line[b->cl] + b->cc, b->len[b->cl] - b->cc + 1);
  free(b->line[b->cl]);
  b->line[b->cl] = l;
  b->len[b->cl] += n;
  b->cc += n;
  b->modified = 1;
}

static int edit_key(struct widget *w, const struct event *e)
{
  struct ebuf *b = w->data;
  int ctrl = (e->shift & 4) != 0, rows = edit_rows(w);
  char *l = b->line[b->cl];

  switch (e->scan)
  {
    case 0x48: b->cl--; break;                             /* up */
    case 0x50: b->cl++; break;                             /* down */
    case 0x4b:                                             /* left */
      if (b->cc > 0) b->cc--;
      else if (b->cl > 0) { b->cl--; b->cc = b->len[b->cl]; }
      break;
    case 0x4d:                                             /* right */
      if (b->cc < b->len[b->cl]) b->cc++;
      else if (b->cl < b->n - 1) { b->cl++; b->cc = 0; }
      break;
    case 0x47: case 0x77: if (ctrl || e->scan == 0x77) b->cl = 0; b->cc = 0; break;  /* home */
    case 0x4f: case 0x75:                                  /* end */
      if (ctrl || e->scan == 0x75) b->cl = b->n - 1;
      b->cc = b->len[b->cl];
      break;
    case 0x49: b->cl -= rows; b->top -= rows; if (b->top < 0) b->top = 0; break;   /* pgup */
    case 0x51: b->cl += rows; b->top += rows; break;       /* pgdn */
    case 0x53:                                             /* delete */
      if (b->cc < b->len[b->cl])
      {
        memmove(l + b->cc, l + b->cc + 1, b->len[b->cl] - b->cc);
        b->len[b->cl]--;
        b->modified = 1;
      }
      else if (b->cl < b->n - 1)                           /* join the next line */
      {
        int n = b->len[b->cl] + b->len[b->cl + 1];
        char *j = malloc(n + 1);
        if (j)
        {
          memcpy(j, l, b->len[b->cl]);
          memcpy(j + b->len[b->cl], b->line[b->cl + 1], b->len[b->cl + 1] + 1);
          eb_set_line(b, b->cl, j, n);
          free(j);
          eb_delete_line(b, b->cl + 1);
          b->modified = 1;
        }
      }
      break;
    default:
      if (e->ascii == 8)                                   /* backspace */
      {
        if (b->cc > 0)
        {
          memmove(l + b->cc - 1, l + b->cc, b->len[b->cl] - b->cc + 1);
          b->len[b->cl]--;
          b->cc--;
          b->modified = 1;
        }
        else if (b->cl > 0)
        {
          int pl = b->len[b->cl - 1], n = pl + b->len[b->cl];
          char *j = malloc(n + 1);
          if (j)
          {
            memcpy(j, b->line[b->cl - 1], pl);
            memcpy(j + pl, l, b->len[b->cl] + 1);
            eb_set_line(b, b->cl - 1, j, n);
            free(j);
            eb_delete_line(b, b->cl);
            b->cl--;
            b->cc = pl;
            b->modified = 1;
          }
        }
      }
      else if (e->ascii == 13)                             /* split the line */
      {
        if (!eb_insert_line(b, b->cl + 1, l + b->cc, b->len[b->cl] - b->cc))
        {
          l = b->line[b->cl];
          l[b->cc] = 0;
          b->len[b->cl] = b->cc;
          b->cl++;
          b->cc = 0;
          b->modified = 1;
        }
      }
      else if (e->ascii == 9)
        edit_insert(b, "    ", TAB_SPACES - b->cc % TAB_SPACES);
      else if (e->ascii >= 32 && e->ascii != 127)
      {
        char c = (char)e->ascii;
        edit_insert(b, &c, 1);
      }
      else
        return 0;
  }
  edit_fix(w);
  if (w->action)
    w->action(w);
  return 1;
}

static void edit_click(struct widget *w, const struct event *e)
{
  struct ebuf *b = w->data;
  int rows = edit_rows(w);

  if (e->x >= w->x + w->w - SCROLL_W - 1)               /* scroll bar: page */
  {
    b->top += e->y < w->y + w->h / 2 ? -rows : rows;
    if (b->top > b->n - rows) b->top = b->n - rows;
    if (b->top < 0) b->top = 0;
    b->cl = b->top;
  }
  else
  {
    b->cl = b->top + (e->y - w->y - 3) / FONT_H;
    if (b->cl >= b->n) b->cl = b->n - 1;
    b->cc = b->left + (e->x - w->x - 5 + FONT_W / 2) / FONT_W;
  }
  edit_fix(w);
  if (w->action)
    w->action(w);
}

static void edit_draw(struct surface *s, struct widget *w, int focused)
{
  struct ebuf *b = w->data;
  int rows = edit_rows(w), cols = edit_cols(w), i;
  char part[256];

  draw_fill(s, w->x, w->y, w->w, w->h, C_FIELD);
  draw_frame(s, w->x, w->y, w->w, w->h, focused ? C_ACCENT : C_LINE);
  surface_clip(s, w->x + 1, w->y + 1, w->w - SCROLL_W - 3, w->h - 2);
  for (i = 0; i < rows && b->top + i < b->n; i++)
  {
    int ln = b->top + i, y = w->y + 3 + i * FONT_H, n = b->len[ln] - b->left;
    if (n > cols + 1)
      n = cols + 1;
    if (n > (int)sizeof(part) - 1)
      n = sizeof(part) - 1;
    if (n < 0)
      n = 0;
    memcpy(part, b->line[ln] + (b->left < b->len[ln] ? b->left : b->len[ln]), n);
    part[n] = 0;
    if (ln == b->cl && focused)
      draw_fill(s, w->x + 2, y, w->w - SCROLL_W - 5, FONT_H, RGB(238, 246, 247));
    draw_text(s, w->x + 5, y, part, C_INK, TEXT_TRANSPARENT);
    if (ln == b->cl && focused)
      draw_vline(s, w->x + 5 + (b->cc - b->left) * FONT_W, y, FONT_H, C_INK);
  }
  surface_noclip(s);
  {                                     /* scroll bar */
    int track = w->h - 2, th = b->n > rows ? track * rows / b->n : track, ty0;
    if (th < 12) th = 12;
    ty0 = w->y + 1 + (b->n > rows ? (track - th) * b->top / (b->n - rows) : 0);
    draw_fill(s, w->x + w->w - SCROLL_W - 1, w->y + 1, SCROLL_W, track, RGB(232, 235, 240));
    draw_fill(s, w->x + w->w - SCROLL_W + 1, ty0 + 1, SCROLL_W - 4, th - 2, RGB(176, 184, 196));
  }
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
    case UI_EDIT:
      edit_draw(s, w, focused);
      break;
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

/* list: Enter or a double click */
static void activate(struct widget *w)
{
  if (w->activate)
    w->activate(w);
  else
    act(w);
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

  if (w && w->type == UI_EDIT && e->ascii == 9)        /* Tab: the editor's */
    return edit_key(w, e);
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
      if (e->ascii == 13) { activate(w); return 1; }
      break;
    case UI_TEXT:
      return text_key(w, e);
    case UI_EDIT:
      return edit_key(w, e);
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
    case UI_EDIT:
      edit_click(w, e);
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
        int again = i == w->value && clock() - w->last_click < CLOCKS_PER_SEC / 2;
        list_select(w, i);
        if (again)
          activate(w);                  /* double click */
        else
          act(w);
        w->last_click = again ? 0 : clock();
      }
      break;
    }
  }
  return 1;
}

static int alive(const struct window *win)
{
  int i;
  for (i = 0; i < wm_count(); i++)
    if (wm_window(i) == win)
      return 1;
  return 0;
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
    if (alive(win))                     /* the action may have closed it */
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

/* ---------------------------------------------------------------- */
/* dialogs                                                          */

struct dialog {
  struct window *win;
  struct widget *field;
  void (*text_done)(const char *text, void *user);
  void (*confirm_done)(int yes, void *user);
  void *user;
};

static void dialog_end(struct dialog *d, int ok)
{
  char text[128];
  void (*td)(const char *, void *) = d->text_done;
  void (*cd)(int, void *) = d->confirm_done;
  void *user = d->user;

  if (d->field)
    strcpy(text, d->field->text);
  wm_window_close(d->win);
  free(d);
  if (td)
    td(ok ? text : 0, user);
  if (cd)
    cd(ok, user);
}

static void dialog_ok(struct widget *w)
{
  dialog_end(w->user, 1);
}

static void dialog_cancel(struct widget *w)
{
  dialog_end(w->user, 0);
}

static int dialog_events(struct window *win, const struct event *e)
{
  struct ui *ui = win->user;

  if (e->type == EV_CLOSE || (e->type == EV_KEY && e->ascii == 27))
  {
    dialog_end(ui->w[ui->n - 1]->user, 0);  /* any widget knows the dialog */
    return 1;
  }
  return 0;
}

static struct dialog *dialog_new(const char *title, const char *text, int h)
{
  struct surface *s = gfx_screen();
  struct dialog *d = calloc(1, sizeof(*d));
  int w = text_width(text) + 48;
  struct ui *ui;

  if (!d)
    return 0;
  if (w < 360)
    w = 360;
  d->win = wm_window_new((s->w - w) / 2, (s->h - h) / 2, w, h, title);
  if (!d->win)
  {
    free(d);
    return 0;
  }
  d->win->handler = dialog_events;
  ui = ui_new(d->win);
  ui_label(ui, 20, 18, text)->user = d;
  return d;
}

static struct widget *dialog_buttons(struct dialog *d, const char *ok, const char *cancel)
{
  struct ui *ui = d->win->user;
  int cw = d->win->client->w, ch = d->win->client->h;
  struct widget *b;

  b = ui_button(ui, cw - 208, ch - 44, 92, 28, ok, dialog_ok);
  b->user = d;
  ui_button(ui, cw - 108, ch - 44, 92, 28, cancel, dialog_cancel)->user = d;
  return b;
}

struct widget *ui_ask_text(const char *title, const char *prompt, const char *initial,
                           void (*done)(const char *text, void *user), void *user)
{
  struct dialog *d = dialog_new(title, prompt, 150);
  struct ui *ui;

  if (!d)
    return 0;
  ui = d->win->user;
  d->text_done = done;
  d->user = user;
  d->field = ui_textfield(ui, 20, 42, d->win->client->w - 40, dialog_ok);
  d->field->user = d;
  ui_set_text(d->field, initial ? initial : "");
  dialog_buttons(d, "OK", "Cancel");
  ui_focus(d->field);
  ui_redraw(ui);
  return d->field;
}

struct widget *ui_confirm(const char *title, const char *text,
                          void (*done)(int yes, void *user), void *user)
{
  struct dialog *d = dialog_new(title, text, 120);
  struct widget *yes;

  if (!d)
    return 0;
  d->confirm_done = done;
  d->user = user;
  yes = dialog_buttons(d, "Yes", "No");
  ui_focus(yes);
  ui_redraw(d->win->user);
  return yes;
}
