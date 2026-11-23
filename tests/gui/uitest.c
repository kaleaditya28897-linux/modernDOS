/*
 * UITEST - mdgui widget toolkit test (DJGPP).
 *
 * A window with a menu bar (File: New, Quit; Help: About), a label, a
 * text field, a checkbox, a scrolling list of 12 items and OK / Cancel
 * buttons. Logs to port E9h:
 *   "UITEST: ready at X,Y", "UITEST: at X,Y" (mouse; the harness steers
 *   by it), "UITEST: widget NAME X,Y" (screen points to click),
 *   "UITEST: ok text=T check=C list=L" (OK button),
 *   "UITEST: menu MENU/ITEM", "UITEST: done"
 * The harness (tests/gui/uidrive.py) types into the field, toggles the
 * checkbox with Tab + Space, picks Item 3, clicks OK, takes a screenshot
 * and quits through File/Quit with the keyboard.
 */
#include <stdio.h>
#include <string.h>
#include <pc.h>
#include "mdgui.h"

static int quit;
static struct widget *text, *check, *list;

static const char *const items[] = {
  "Item 1", "Item 2", "Item 3", "Item 4", "Item 5", "Item 6",
  "Item 7", "Item 8", "Item 9", "Item 10", "Item 11", "Item 12",
};
static const char *const file_items[] = { "New", "Quit" };
static const char *const help_items[] = { "About" };
static const struct ui_menu menus[] = {
  { "File", file_items, 2 },
  { "Help", help_items, 1 },
};

static void out(const char *s)
{
  for (; *s; s++)
  {
    if (*s == '\n')
      outportb(0xe9, '\r');
    outportb(0xe9, *s);
  }
}

static void ok(struct widget *w)
{
  char buf[200];

  (void)w;
  sprintf(buf, "UITEST: ok text=%s check=%d list=%s\n", text->text, check->value,
          list->value >= 0 ? items[list->value] : "none");
  out(buf);
}

static void cancel(struct widget *w)
{
  (void)w;
  out("UITEST: cancel\n");
}

static void menu(struct widget *w)
{
  char buf[96];
  const struct ui_menu *m = &w->menus[w->menu];

  sprintf(buf, "UITEST: menu %s/%s\n", m->title, m->items[w->value]);
  out(buf);
  if (!strcmp(m->items[w->value], "Quit"))
    quit = 1;
}

static int window_events(struct window *w, const struct event *e)
{
  (void)w;
  if (e->type == EV_CLOSE)
  {
    quit = 1;
    return 1;
  }
  return 0;
}

static void widget_at(const char *name, struct widget *w, int dx, int dy)
{
  char buf[96];
  int x, y;

  ui_widget_center(w, &x, &y);
  sprintf(buf, "UITEST: widget %s %d,%d\n", name, x + dx, y + dy);
  out(buf);
}

int main(void)
{
  struct surface *s;
  struct window *win;
  struct widget *bar, *okb;
  struct ui *ui;
  char buf[96];

  out("\nwidget test\n");
  if (gfx_open(1024, 768))
  {
    sprintf(buf, "UITEST: failed: %s\n", gfx_error());
    out(buf);
    return 1;
  }
  s = gfx_screen();
  input_open();
  wm_init();
  win = wm_window_new(150, 90, 480, 380, "Widgets");
  win->handler = window_events;
  ui = ui_new(win);
  bar = ui_menubar(ui, menus, 2, menu);
  ui_label(ui, 16, 40, "Name:");
  text = ui_textfield(ui, 80, 36, 220, 0);
  check = ui_check(ui, 16, 76, "Subscribe to news", 0);
  list = ui_list(ui, 16, 110, 200, 124, items, 12, 0);
  okb = ui_button(ui, 240, 110, 100, 30, "OK", ok);
  ui_button(ui, 240, 150, 100, 30, "Cancel", cancel);
  ui_label(ui, 16, 250, "Tab moves the focus, F10 opens the menu.");
  ui_focus(text);
  ui_redraw(ui);
  wm_update();

  widget_at("text", text, 0, 0);
  widget_at("checkbox", check, -check->w / 2 + 4, -check->h / 2 + 5);   /* in the box */
  widget_at("item3", list, 0, -list->h / 2 + 2 + 2 * 20 + 10);
  widget_at("ok", okb, 0, 0);
  widget_at("file", bar, -bar->w / 2 + 8 + text_width("File") / 2, 0);
  gfx_cursor_move(s->w / 2, s->h / 2);
  gfx_cursor_show(1);
  sprintf(buf, "UITEST: ready at %d,%d\n", s->w / 2, s->h / 2);
  out(buf);

  while (!quit)
  {
    struct event e;
    input_wait(&e);
    if (e.type == EV_MOUSE_MOVE)
    {
      sprintf(buf, "UITEST: at %d,%d\n", e.x, e.y);
      out(buf);
    }
    wm_dispatch(&e);
    wm_update();
  }
  input_close();
  gfx_close();
  out("UITEST: done\n");
  return 0;
}
