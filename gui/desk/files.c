/*
 * MDDESK Files - the file manager.
 *
 * A window per folder view: the path (type one and press Enter: a folder
 * or a drive such as C:), buttons (Up, Refresh, New folder, Rename,
 * Delete, Open) and the folder's contents, folders first, with long file
 * names, sizes and dates. Enter or a double click opens: a folder is
 * entered, a program (.EXE .COM .BAT) is run through the desktop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "desk.h"

#define MAX_ENTRIES 512
#define NAME_COLS   38

struct entry {
  char name[256];
  int dir;
  long size;
  time_t mtime;
};

struct files {
  struct window *win;
  struct ui *ui;
  struct widget *path_field, *list, *status;
  char path[260];                       /* always ends in a backslash */
  struct entry *entries;
  int n;
  char **items;
  char status_text[256];
};

/* ---------------------------------------------------------------- */

static void join(char *out, const char *dir, const char *name)
{
  strcpy(out, dir);
  strcat(out, name);
}

static int compare(const void *a, const void *b)
{
  const struct entry *x = a, *y = b;
  if (x->dir != y->dir)
    return y->dir - x->dir;              /* folders first */
  if (!strcmp(x->name, ".."))
    return -1;
  if (!strcmp(y->name, ".."))
    return 1;
  return strcasecmp(x->name, y->name);
}

static void status(struct files *f, const char *text)
{
  snprintf(f->status_text, sizeof(f->status_text), "%s", text);
  ui_set_text(f->status, f->status_text);
}

static void format_item(char *out, const struct entry *e)
{
  char name[NAME_COLS + 1], size[16], date[64];
  struct tm *tm = localtime(&e->mtime);

  strncpy(name, e->name, NAME_COLS);
  name[NAME_COLS] = 0;
  if ((int)strlen(e->name) > NAME_COLS)
    name[NAME_COLS - 1] = '~';
  if (e->dir)
    strcpy(size, "<folder>");
  else
    sprintf(size, "%ld", e->size);
  if (strcmp(e->name, "..") && tm)
    sprintf(date, "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min);
  else
    date[0] = 0;
  snprintf(out, 160, "%-*s %9s  %.16s", NAME_COLS, name, size, date);
}

/* read the folder f->path into the list */
static void refresh(struct files *f)
{
  DIR *d;
  struct dirent *de;
  char full[520], line[160];
  int i, n = 0;

  for (i = 0; i < f->n; i++)
    free(f->items[i]);
  f->n = 0;
  d = opendir(f->path);
  if (!d)
  {
    sprintf(line, "Cannot open %s", f->path);
    status(f, line);
  }
  while (d && (de = readdir(d)) && n < MAX_ENTRIES)
  {
    struct entry *e = &f->entries[n];
    struct stat st;

    if (!strcmp(de->d_name, "."))
      continue;
    if (!strcmp(de->d_name, "..") && strlen(f->path) <= 3)
      continue;                         /* no .. in a root folder */
    strncpy(e->name, de->d_name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    join(full, f->path, e->name);
    if (stat(full, &st) == 0)
    {
      e->dir = S_ISDIR(st.st_mode);
      e->size = st.st_size;
      e->mtime = st.st_mtime;
    }
    else
    {
      e->dir = !strcmp(e->name, "..");
      e->size = 0;
      e->mtime = 0;
    }
    n++;
  }
  if (d)
    closedir(d);
  if (n > 1)
    qsort(f->entries, n, sizeof(*f->entries), compare);
  for (i = 0; i < n; i++)
  {
    format_item(line, &f->entries[i]);
    f->items[i] = strdup(line);
  }
  f->n = n;
  f->list->items = (const char *const *)f->items;
  f->list->nitems = n;
  f->list->value = n ? 0 : -1;
  f->list->scroll = 0;
  ui_set_text(f->path_field, f->path);
  sprintf(line, "%d item%s", n, n == 1 ? "" : "s");
  status(f, line);
  sprintf(f->win->title, "Files - %s", f->path);
  wm_invalidate(f->win);
  panel_damage();

  if (test_mode)                        /* the listing, and where its items are */
  {
    char *buf = malloc(n * 260 + 64), *p = buf;
    int rows = (f->list->h - 4) / 20;
    p += sprintf(p, "MDDESK: files %s %d:", f->path, n);
    for (i = 0; i < n; i++)
      p += sprintf(p, "%s%s", i ? "|" : " ", f->entries[i].name);
    tlog("%s\n", buf);
    free(buf);
    for (i = 0; i < n && i < rows; i++)
    {
      int x, y;
      ui_widget_center(f->list, &x, &y);
      tlog("MDDESK: fileitem %s %d,%d\n", f->entries[i].name, x - 100,
           y - f->list->h / 2 + 2 + i * 20 + 10);
    }
  }
}

static void go(struct files *f, const char *path)
{
  char p[260];
  size_t len;

  strncpy(p, path, sizeof(p) - 2);
  p[sizeof(p) - 2] = 0;
  len = strlen(p);
  if (len == 2 && p[1] == ':')          /* "C:" means the root */
    strcat(p, "\\");
  len = strlen(p);
  if (len && p[len - 1] != '\\' && p[len - 1] != '/')
    strcat(p, "\\");
  strcpy(f->path, p);
  refresh(f);
}

static void up(struct files *f)
{
  char *p;

  if (strlen(f->path) <= 3)
    return;
  f->path[strlen(f->path) - 1] = 0;     /* drop the last backslash */
  p = strrchr(f->path, '\\');
  if (p)
    p[1] = 0;
  refresh(f);
}

static struct entry *selected(struct files *f)
{
  return f->list->value >= 0 && f->list->value < f->n ? &f->entries[f->list->value] : 0;
}

static int is_program(const char *name)
{
  const char *dot = strrchr(name, '.');
  return dot && (!strcasecmp(dot, ".exe") || !strcasecmp(dot, ".com") || !strcasecmp(dot, ".bat"));
}

static void open_entry(struct files *f)
{
  struct entry *e = selected(f);
  char full[520];

  if (!e)
    return;
  if (!strcmp(e->name, ".."))
  {
    up(f);
    return;
  }
  join(full, f->path, e->name);
  if (e->dir)
    go(f, full);
  else if (is_program(e->name))
  {
    char cmd[530];
    sprintf(cmd, strchr(full, ' ') ? "\"%s\"" : "%s", full);
    desk_run(cmd);
  }
  else
    status(f, "No application for this file yet.");
}

/* ---------------------------------------------------------------- */
/* buttons                                                          */

static void b_up(struct widget *w) { up(w->user); }
static void b_refresh(struct widget *w) { refresh(w->user); }
static void b_open(struct widget *w) { open_entry(w->user); }
static void l_activate(struct widget *w) { open_entry(w->user); }
static void p_enter(struct widget *w) { go(w->user, w->text); }

static void new_folder_done(const char *name, void *user)
{
  struct files *f = user;
  char full[520];

  if (!name || !name[0])
    return;
  join(full, f->path, name);
  if (mkdir(full, 0777))
  {
    status(f, "Cannot create the folder.");
    return;
  }
  refresh(f);
  tlog("MDDESK: created %s\n", full);
}

static void b_new_folder(struct widget *w)
{
  struct widget *field = ui_ask_text("New folder", "Name of the new folder:", "", new_folder_done,
                                     w->user);
  if (field)
    desk_widget("dialogfield", field);
}

static char rename_from[256];

static void rename_done(const char *name, void *user)
{
  struct files *f = user;
  char from[520], to[520];

  if (!name || !name[0] || !strcmp(name, rename_from))
    return;
  join(from, f->path, rename_from);
  join(to, f->path, name);
  if (rename(from, to))
  {
    status(f, "Cannot rename.");
    return;
  }
  refresh(f);
  tlog("MDDESK: renamed %s to %s\n", rename_from, name);
}

static void b_rename(struct widget *w)
{
  struct files *f = w->user;
  struct entry *e = selected(f);
  struct widget *field;

  if (!e || !strcmp(e->name, ".."))
    return;
  strcpy(rename_from, e->name);
  field = ui_ask_text("Rename", "New name:", e->name, rename_done, f);
  if (field)
    desk_widget("dialogfield", field);
}

static void delete_done(int yes, void *user)
{
  struct files *f = user;
  struct entry *e = selected(f);
  char full[520];

  if (!yes || !e)
    return;
  join(full, f->path, e->name);
  if (e->dir ? rmdir(full) : remove(full))
  {
    status(f, e->dir ? "Cannot delete (is the folder empty?)." : "Cannot delete.");
    return;
  }
  tlog("MDDESK: deleted %s\n", full);
  refresh(f);
}

static void b_delete(struct widget *w)
{
  struct files *f = w->user;
  struct entry *e = selected(f);
  char text[300];
  struct widget *yes;

  if (!e || !strcmp(e->name, ".."))
    return;
  sprintf(text, "Delete %s \"%.200s\"?", e->dir ? "the folder" : "the file", e->name);
  yes = ui_confirm("Delete", text, delete_done, f);
  if (yes)
    desk_widget("confirmyes", yes);
}

#define MAX_VIEWS 16
static struct files *views[MAX_VIEWS];  /* open file manager windows */

static int files_events(struct window *w, const struct event *e)
{
  struct files *f = 0;
  int i;

  if (e->type != EV_CLOSE)
    return 0;
  for (i = 0; i < MAX_VIEWS; i++)
    if (views[i] && views[i]->win == w)
    {
      f = views[i];
      views[i] = 0;
    }
  if (!f)
    return 0;
  for (i = 0; i < f->n; i++)
    free(f->items[i]);
  free(f->items);
  free(f->entries);
  wm_window_close(w);
  free(f);
  panel_damage();
  return 1;
}

void files_open(const char *path)
{
  static int count;
  struct files *f = calloc(1, sizeof(*f));
  struct widget *b;
  char cwd[260];
  int x = 12, bw = 92, i;
  static const struct { const char *text; ui_action a; const char *name; } buttons[] = {
    { "Up", b_up, "files_up" }, { "Refresh", b_refresh, "files_refresh" },
    { "New folder", b_new_folder, "files_newfolder" }, { "Rename", b_rename, "files_rename" },
    { "Delete", b_delete, "files_delete" }, { "Open", b_open, "files_open" },
  };

  if (!f)
    return;
  f->entries = calloc(MAX_ENTRIES, sizeof(*f->entries));
  f->items = calloc(MAX_ENTRIES, sizeof(char *));
  f->win = wm_window_new(60 + 24 * (count % 6), 50 + 24 * (count % 6), 640, 440, "Files");
  count++;
  if (!f->entries || !f->items || !f->win)
    return;
  for (i = 0; i < MAX_VIEWS && views[i]; i++)
    ;
  if (i == MAX_VIEWS)
  {
    wm_window_close(f->win);
    free(f->entries);
    free(f->items);
    free(f);
    return;
  }
  views[i] = f;
  f->win->handler = files_events;
  f->ui = ui_new(f->win);
  ui_label(f->ui, 12, 14, "Path:");
  f->path_field = ui_textfield(f->ui, 60, 10, 560, p_enter);
  f->path_field->user = f;
  for (i = 0; i < 6; i++)
  {
    b = ui_button(f->ui, x, 44, bw, 26, buttons[i].text, buttons[i].a);
    b->user = f;
    desk_widget(buttons[i].name, b);
    x += bw + 8;
  }
  f->list = ui_list(f->ui, 12, 80, 612, 284, 0, 0, 0);
  f->list->user = f;
  f->list->activate = l_activate;
  f->status = ui_label(f->ui, 12, 374, "");
  desk_widget("files_path", f->path_field);
  if (!path)
  {
    getcwd(cwd, sizeof(cwd));
    cwd[3] = 0;                         /* the root of the current drive */
    path = cwd;
  }
  for (i = 0; path[i] && i < 259; i++)
    cwd[i] = path[i] == '/' ? '\\' : path[i];
  cwd[i] = 0;
  cwd[0] = (char)toupper((unsigned char)cwd[0]);
  ui_focus(f->list);
  go(f, cwd);
  tlog("MDDESK: window Files at %d,%d\n", f->win->x, f->win->y);
}
