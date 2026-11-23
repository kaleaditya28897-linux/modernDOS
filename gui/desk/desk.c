/*
 * MDDESK - the modernDOS desktop (DJGPP, mdgui).
 *
 * A panel at the top: the launcher (modernDOS menu), a button for every
 * open window (raises it) and a clock. The launcher has About, Run (a DOS
 * program: the desktop goes to text mode, runs it through COMMAND.COM,
 * waits for a key and comes back) and Exit to DOS.
 *
 *   MDDESK [/TEST]   /TEST logs positions and events to port E9h for the
 *                    test harness (tests/gui/deskdrive.py)
 *
 * Not yet: starting DJGPP (32-bit DPMI) programs from the desktop - the
 * DPMI host runs one client at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <pc.h>
#include <dpmi.h>
#include <conio.h>
#include "desk.h"

#define VERSION   "0.2"
#define TASK_W    150

#define C_PANEL      RGB(24, 30, 40)
#define C_PANEL_LINE RGB(48, 58, 74)
#define C_PANEL_TEXT RGB(220, 226, 234)
#define C_TASK       RGB(44, 52, 66)
#define C_TASK_TOP   RGB(32, 128, 136)

int test_mode;
static int quit, menu_open_now;
static char clock_text[8];
static char last_tasks[256];
static struct window *about_win, *run_win;
static struct widget *run_field;

void tlog(const char *fmt, ...)
{
  static char buf[8192];
  char *p;
  va_list ap;

  if (!test_mode)
    return;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (p = buf; *p; p++)
  {
    if (*p == '\n')
      outportb(0xe9, '\r');
    outportb(0xe9, *p);
  }
}

/* ---------------------------------------------------------------- */
/* panel                                                            */

static int launcher_w(void)
{
  return text_width("modernDOS") + 28;
}

void panel_damage(void)
{
  wm_damage(0, 0, gfx_screen()->w, PANEL_H);
}

static void draw_panel(struct surface *s)
{
  struct window *top = wm_top();
  int i, x = launcher_w() + 16, n = 0;
  char tasks[256] = "";

  draw_fill(s, 0, 0, s->w, PANEL_H, C_PANEL);
  draw_hline(s, 0, PANEL_H - 1, s->w, C_PANEL_LINE);
  /* launcher */
  draw_fill(s, 6, 4, launcher_w(), PANEL_H - 8, menu_open_now ? C_TASK_TOP : C_TASK);
  draw_fill(s, 14, PANEL_H / 2 - 4, 8, 8, RGB(64, 200, 200));
  draw_text(s, 30, (PANEL_H - FONT_H) / 2, "modernDOS", C_PANEL_TEXT, TEXT_TRANSPARENT);
  /* open windows */
  for (i = 0; i < wm_count(); i++)
  {
    struct window *w = wm_window(i);
    char t[20];
    if (w->popup || x + TASK_W > s->w - 80)
      continue;
    strncpy(t, w->title, 17);
    t[17] = 0;
    draw_fill(s, x, 4, TASK_W, PANEL_H - 8, w == top ? C_TASK_TOP : C_TASK);
    draw_text(s, x + 8, (PANEL_H - FONT_H) / 2, t, C_PANEL_TEXT, TEXT_TRANSPARENT);
    n += sprintf(tasks + n, " %s=%d,%d", w->title, x + TASK_W / 2, PANEL_H / 2);
    x += TASK_W + 6;
  }
  /* clock */
  draw_text(s, s->w - text_width(clock_text) - 14, (PANEL_H - FONT_H) / 2, clock_text,
            C_PANEL_TEXT, TEXT_TRANSPARENT);
  if (strcmp(tasks, last_tasks))
  {
    strcpy(last_tasks, tasks);
    tlog("MDDESK: taskbar%s\n", tasks);
  }
}

void desk_widget(const char *name, const struct widget *w)
{
  int x, y;

  if (!test_mode)
    return;
  ui_widget_center(w, &x, &y);
  tlog("MDDESK: widget %s %d,%d\n", name, x, y);
}

static void tick_clock(void)
{
  time_t t = time(0);
  struct tm *tm = localtime(&t);
  char now[8];

  sprintf(now, "%02d:%02d", tm->tm_hour, tm->tm_min);
  if (strcmp(now, clock_text))
  {
    strcpy(clock_text, now);
    panel_damage();
    tlog("MDDESK: clock %s\n", clock_text);
  }
}

/* ---------------------------------------------------------------- */
/* windows                                                          */

static int close_handler(struct window *w, const struct event *e)
{
  if (e->type == EV_CLOSE)
  {
    if (w == about_win)
      about_win = 0;
    if (w == run_win)
      run_win = 0;
    wm_window_close(w);
    panel_damage();
    return 1;
  }
  return 0;
}

static void about_ok(struct widget *w)
{
  (void)w;
  if (about_win)
  {
    wm_window_close(about_win);
    about_win = 0;
    panel_damage();
  }
}

static void open_about(void)
{
  const struct gfx_mode *m = gfx_mode();
  __dpmi_free_mem_info mi;
  __dpmi_regs r;
  struct ui *ui;
  struct widget *ok;
  char line[96];
  int x, y, mdvmm, sessions = 0;

  if (about_win)
  {
    wm_raise(about_win);
    return;
  }
  about_win = wm_window_new(260, 140, 420, 250, "About modernDOS");
  if (!about_win)
    return;
  about_win->handler = close_handler;
  ui = ui_new(about_win);
  ui_label(ui, 20, 20, "modernDOS desktop " VERSION);
  ui_label(ui, 20, 40, "DOS, reimagined as if it never faded.");
  sprintf(line, "Display: %dx%d, VBE mode %Xh", m->w, m->h, m->vbe_mode);
  ui_label(ui, 20, 76, strdup(line));
  memset(&r, 0, sizeof(r));
  r.x.ax = 0xde00;
  __dpmi_int(0x2f, &r);
  mdvmm = r.h.al == 0xff && r.x.bx == 0x4d44;
  if (mdvmm)
  {
    memset(&r, 0, sizeof(r));
    r.x.ax = 0xde06;
    __dpmi_int(0x2f, &r);
    sessions = r.x.dx ? r.x.cx : 0;
  }
  sprintf(line, "Boot: %s", !mdvmm ? "DOS, no MDVMM" :
          m->vbe_mode == 0x141 ? "UEFI, MDVMM virtual BIOS" : "BIOS, MDVMM");
  ui_label(ui, 20, 96, strdup(line));
  tlog("MDDESK: about %s, sessions %d\n", line, sessions);
  __dpmi_get_free_memory_information(&mi);
  sprintf(line, "Free memory: %lu KB, DOS sessions: %d", mi.largest_available_free_block_in_bytes / 1024,
          sessions);
  ui_label(ui, 20, 116, strdup(line));
  ok = ui_button(ui, 300, 170, 90, 28, "OK", about_ok);
  ui_focus(ok);
  ui_redraw(ui);
  ui_widget_center(ok, &x, &y);
  tlog("MDDESK: window About at %d,%d\n", about_win->x, about_win->y);
  tlog("MDDESK: widget aboutok %d,%d\n", x, y);
  panel_damage();
}

void desk_run(const char *cmd)
{
  int rc;

  tlog("MDDESK: running %s\n", cmd);
  input_close();
  gfx_suspend();
  printf("modernDOS desktop: running %s\n\n", cmd);
  fflush(stdout);
  rc = system(cmd);
  printf("\n[exit code %d] Press a key to return to the desktop.", rc);
  fflush(stdout);
  tlog("MDDESK: ran %s exit %d\n", cmd, rc);
  while (kbhit())
    getch();
  getch();
  gfx_resume();
  input_open();
  gfx_cursor_show(1);
  wm_damage(0, 0, gfx_screen()->w, gfx_screen()->h);
  tlog("MDDESK: resumed\n");
}

static void run_go(struct widget *w)
{
  char cmd[128];

  (void)w;
  if (!run_win)
    return;
  strcpy(cmd, run_field->text);
  wm_window_close(run_win);
  run_win = 0;
  panel_damage();
  wm_update();
  if (cmd[0])
    desk_run(cmd);
}

static void run_cancel(struct widget *w)
{
  (void)w;
  if (run_win)
  {
    wm_window_close(run_win);
    run_win = 0;
    panel_damage();
  }
}

static void open_run(void)
{
  struct ui *ui;
  struct widget *go;
  int x, y;

  if (run_win)
  {
    wm_raise(run_win);
    return;
  }
  run_win = wm_window_new(300, 200, 400, 150, "Run a DOS program");
  if (!run_win)
    return;
  run_win->handler = close_handler;
  ui = ui_new(run_win);
  ui_label(ui, 16, 20, "Command:");
  run_field = ui_textfield(ui, 96, 16, 280, run_go);
  go = ui_button(ui, 176, 70, 96, 28, "Run", run_go);
  ui_button(ui, 280, 70, 96, 28, "Cancel", run_cancel);
  ui_focus(run_field);
  ui_redraw(ui);
  ui_widget_center(run_field, &x, &y);
  tlog("MDDESK: widget runfield %d,%d\n", x, y);
  ui_widget_center(go, &x, &y);
  tlog("MDDESK: widget runbutton %d,%d\n", x, y);
  panel_damage();
}

/* ---------------------------------------------------------------- */
/* launcher                                                         */

static const char *const launcher_items[] = { "About modernDOS", "Files", "Run...", "Exit to DOS" };
static const char *const launcher_names[] = { "about", "files", "run", "exit" };
#define NLAUNCH 4

static void launcher_choose(int item, void *user)
{
  (void)user;
  menu_open_now = 0;
  panel_damage();
  switch (item)
  {
    case 0: open_about(); break;
    case 1: files_open(0); break;
    case 2: open_run(); break;
    case 3: quit = 1; break;
  }
}

static void open_launcher(void)
{
  int i;

  menu_open_now = 1;
  panel_damage();
  ui_popup(6, PANEL_H - 2, launcher_items, NLAUNCH, launcher_choose, 0);
  for (i = 0; i < NLAUNCH; i++)
    tlog("MDDESK: item %s %d,%d\n", launcher_names[i], 6 + 40, PANEL_H - 2 + 1 + 2 + i * 20 + 10);
}

static int desk_event(const struct event *e)
{
  int i, x = launcher_w() + 16;

  if (e->type == EV_KEY && e->scan == 0x44)       /* F10 */
  {
    open_launcher();
    return 1;
  }
  if (e->type != EV_MOUSE_DOWN || e->y >= PANEL_H)
    return 0;
  if (e->x >= 6 && e->x < 6 + launcher_w())
  {
    open_launcher();
    return 1;
  }
  for (i = 0; i < wm_count(); i++)
  {
    struct window *w = wm_window(i);
    if (w->popup)
      continue;
    if (e->x >= x && e->x < x + TASK_W)
    {
      wm_raise(w);
      panel_damage();
      return 1;
    }
    x += TASK_W + 6;
  }
  return 0;
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
  struct surface *s;
  struct window *top = 0;
  struct event e;
  __dpmi_regs r;
  int i;

  for (i = 1; i < argc; i++)
    if (!strcasecmp(argv[i], "/TEST"))
      test_mode = 1;
  if (gfx_open(1024, 768))
  {
    printf("MDDESK: %s\n", gfx_error());
    return 1;
  }
  s = gfx_screen();
  input_open();
  wm_init();
  tick_clock();
  wm_desktop(PANEL_H, draw_panel, desk_event);
  wm_update();
  gfx_cursor_move(s->w / 2, s->h / 2);
  gfx_cursor_show(1);
  tlog("MDDESK: widget launcher %d,%d\n", 6 + launcher_w() / 2, PANEL_H / 2);
  tlog("MDDESK: ready at %d,%d\n", s->w / 2, s->h / 2);

  while (!quit)
  {
    if (wm_top() != top)                /* the panel shows the top window */
    {
      top = wm_top();
      panel_damage();
    }
    wm_update();
    if (!input_poll(&e))
    {
      tick_clock();
      memset(&r, 0, sizeof(r));
      r.x.ax = 0x1680;                  /* idle */
      __dpmi_int(0x2f, &r);
      continue;
    }
    if (e.type == EV_MOUSE_MOVE)
      tlog("MDDESK: at %d,%d\n", e.x, e.y);
    wm_dispatch(&e);
  }
  input_close();
  gfx_close();
  tlog("MDDESK: exit\n");
  printf("modernDOS desktop closed.\n");
  return 0;
}
