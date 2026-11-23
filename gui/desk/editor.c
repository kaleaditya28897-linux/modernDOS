/*
 * MDDESK Editor - a text editor.
 *
 * A window per file: a menu bar (File: New, Open..., Save, Save as...,
 * Close), the text (the toolkit's UI_EDIT widget) and a status line with
 * the caret position and the modified mark. Ctrl+S saves. Files are read
 * with LF or CRLF line ends and written with CRLF. Closing with unsaved
 * changes asks first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "desk.h"

#define MAX_FILE (1024L * 1024L)
#define MAX_EDITORS 8

struct editor {
  struct window *win;
  struct ui *ui;
  struct widget *bar, *text, *status;
  char path[260];                       /* "" for a new file */
  char status_text[160];
};

static struct editor *editors[MAX_EDITORS];

static const char *const file_items[] = { "New", "Open...", "Save", "Save as...", "Close" };
static const struct ui_menu menus[] = { { "File", file_items, 5 } };

static struct editor *find(const struct window *w)
{
  int i;
  for (i = 0; i < MAX_EDITORS; i++)
    if (editors[i] && editors[i]->win == w)
      return editors[i];
  return 0;
}

static void update_title(struct editor *ed)
{
  const char *name = ed->path[0] ? strrchr(ed->path, '\\') : 0;

  snprintf(ed->win->title, sizeof(ed->win->title), "%s%.48s - Editor",
           edit_modified(ed->text) ? "*" : "",
           ed->path[0] ? (name ? name + 1 : ed->path) : "Untitled");
  wm_invalidate(ed->win);
  panel_damage();
}

static void update_status(struct editor *ed)
{
  int line, col;

  edit_caret(ed->text, &line, &col);
  snprintf(ed->status_text, sizeof(ed->status_text), "Line %d of %d, column %d%s", line,
           edit_lines(ed->text), col, edit_modified(ed->text) ? "   modified" : "");
  ui_set_text(ed->status, ed->status_text);
}

static void changed(struct widget *w)
{
  struct editor *ed = w->user;
  static int was_modified;

  update_status(ed);
  if (edit_modified(w) != was_modified)
  {
    was_modified = edit_modified(w);
    update_title(ed);
  }
}

static int load(struct editor *ed, const char *path)
{
  FILE *f = fopen(path, "rb");
  long size;
  char *buf;

  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0 || size > MAX_FILE || !(buf = malloc(size + 1)))
  {
    fclose(f);
    return -1;
  }
  size = (long)fread(buf, 1, size, f);
  fclose(f);
  edit_set_text(ed->text, buf, size);
  free(buf);
  strncpy(ed->path, path, sizeof(ed->path) - 1);
  update_title(ed);
  update_status(ed);
  tlog("MDDESK: editor opened %s, %d lines\n", path, edit_lines(ed->text));
  return 0;
}

static int save(struct editor *ed)
{
  long len;
  char *text = edit_get_text(ed->text, &len);
  FILE *f;
  int ok;

  if (!text)
    return -1;
  f = fopen(ed->path, "wb");
  ok = f && fwrite(text, 1, len, f) == (size_t)len;
  if (f && fclose(f))
    ok = 0;
  if (!ok)
  {
    free(text);
    ui_set_text(ed->status, "Cannot save the file.");
    return -1;
  }
  edit_set_modified(ed->text, 0);
  update_title(ed);
  update_status(ed);
  tlog("MDDESK: editor saved %s, %ld bytes: %s\n", ed->path, len, text);
  free(text);
  return 0;
}

/* ---------------------------------------------------------------- */

static void close_editor(struct editor *ed)
{
  int i;

  for (i = 0; i < MAX_EDITORS; i++)
    if (editors[i] == ed)
      editors[i] = 0;
  wm_window_close(ed->win);
  free(ed);
  panel_damage();
  tlog("MDDESK: editor closed\n");
}

static void discard_done(int yes, void *user)
{
  if (yes)
    close_editor(user);
}

static void ask_close(struct editor *ed)
{
  struct widget *yes;

  if (!edit_modified(ed->text))
  {
    close_editor(ed);
    return;
  }
  yes = ui_confirm("Editor", "The text has changed. Close without saving?", discard_done, ed);
  if (yes)
    desk_widget("confirmyes", yes);
}

static void open_done(const char *path, void *user)
{
  struct editor *ed = user;

  if (path && path[0] && load(ed, path))
    ui_set_text(ed->status, "Cannot open the file.");
}

static void save_as_done(const char *path, void *user)
{
  struct editor *ed = user;

  if (!path || !path[0])
    return;
  strncpy(ed->path, path, sizeof(ed->path) - 1);
  save(ed);
}

static void menu(struct widget *w)
{
  struct editor *ed = w->user;
  struct widget *field;

  switch (w->value)
  {
    case 0:                             /* New */
      editor_open(0);
      break;
    case 1:                             /* Open... */
      field = ui_ask_text("Open", "File to open:", ed->path, open_done, ed);
      if (field)
        desk_widget("dialogfield", field);
      break;
    case 2:                             /* Save */
      if (ed->path[0])
      {
        save(ed);
        break;
      }
      __attribute__((fallthrough));     /* no name yet: Save as */
    case 3:                             /* Save as... */
      field = ui_ask_text("Save as", "Save the text as:", ed->path, save_as_done, ed);
      if (field)
        desk_widget("dialogfield", field);
      break;
    case 4:
      ask_close(ed);
      break;
  }
}

static int editor_events(struct window *win, const struct event *e)
{
  struct editor *ed = find(win);

  if (!ed)
    return 0;
  if (e->type == EV_CLOSE)
  {
    ask_close(ed);
    return 1;
  }
  if (e->type == EV_KEY && e->ascii == 0x13)    /* Ctrl+S */
  {
    ed->bar->value = 2;
    menu(ed->bar);
    return 1;
  }
  return 0;
}

void editor_open(const char *path)
{
  static int count;
  struct editor *ed = calloc(1, sizeof(*ed));
  int i, cw, ch;

  if (!ed)
    return;
  for (i = 0; i < MAX_EDITORS && editors[i]; i++)
    ;
  if (i == MAX_EDITORS)
  {
    free(ed);
    return;
  }
  ed->win = wm_window_new(120 + 24 * (count % 5), 70 + 24 * (count % 5), 660, 480, "Editor");
  count++;
  if (!ed->win)
  {
    free(ed);
    return;
  }
  editors[i] = ed;
  ed->win->handler = editor_events;
  ed->ui = ui_new(ed->win);
  cw = ed->win->client->w;
  ch = ed->win->client->h;
  ed->bar = ui_menubar(ed->ui, menus, 1, menu);
  ed->bar->user = ed;
  ed->text = ui_edit(ed->ui, 6, 28, cw - 12, ch - 28 - 30, changed);
  ed->text->user = ed;
  ed->status = ui_label(ed->ui, 8, ch - 22, "");
  ui_focus(ed->text);
  if (!path || load(ed, path))
  {
    if (path)
      strncpy(ed->path, path, sizeof(ed->path) - 1);
    update_title(ed);
    update_status(ed);
  }
  ui_redraw(ed->ui);
  tlog("MDDESK: window Editor at %d,%d\n", ed->win->x, ed->win->y);
}
