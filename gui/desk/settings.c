/*
 * MDDESK Settings - the desktop's look and behaviour.
 *
 * Background (a gradient preset), accent colour (active title bars,
 * focus, selection), 12 or 24 hour clock, mouse speed. Changes apply at
 * once; Save writes MDDESK.CFG next to MDDESK.EXE (key=value lines),
 * which the desktop reads when it starts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desk.h"

struct preset_bg { const char *name; color_t top, bottom; };
struct preset_accent { const char *name; color_t accent, hi, soft; };

static const struct preset_bg backgrounds[] = {
  { "Slate", RGB(36, 52, 71), RGB(18, 28, 40) },
  { "Ocean", RGB(24, 72, 112), RGB(8, 28, 52) },
  { "Forest", RGB(34, 70, 48), RGB(14, 30, 22) },
  { "Plum", RGB(70, 40, 72), RGB(28, 16, 30) },
  { "Charcoal", RGB(40, 42, 46), RGB(40, 42, 46) },
};
static const struct preset_accent accents[] = {
  { "Teal", RGB(32, 128, 136), RGB(64, 168, 176), RGB(238, 246, 247) },
  { "Blue", RGB(40, 96, 176), RGB(80, 136, 216), RGB(236, 242, 250) },
  { "Orange", RGB(196, 104, 32), RGB(230, 146, 72), RGB(250, 242, 234) },
  { "Purple", RGB(120, 72, 160), RGB(160, 112, 200), RGB(244, 238, 250) },
  { "Green", RGB(48, 132, 72), RGB(88, 172, 112), RGB(236, 246, 238) },
};
static const char *const speeds[] = { "Slow", "Normal", "Fast" };
static const int speed_mickeys[] = { 16, 8, 4 };

#define NBG     (int)(sizeof(backgrounds) / sizeof(backgrounds[0]))
#define NACCENT (int)(sizeof(accents) / sizeof(accents[0]))
#define NSPEED  3

static struct {
  int bg, accent, clock24, speed;
} cfg = { 0, 0, 1, 1 };

static char cfg_path[260] = "MDDESK.CFG";
static const char *bg_names[NBG], *accent_names[NACCENT];
static struct window *win;
static struct widget *check24;

static void describe(char *out, size_t n)
{
  snprintf(out, n, "background=%s accent=%s clock=%d mouse=%s", backgrounds[cfg.bg].name,
           accents[cfg.accent].name, cfg.clock24 ? 24 : 12, speeds[cfg.speed]);
}

static void apply(void)
{
  theme.desk_top = backgrounds[cfg.bg].top;
  theme.desk_bottom = backgrounds[cfg.bg].bottom;
  theme.accent = accents[cfg.accent].accent;
  theme.accent_hi = accents[cfg.accent].hi;
  theme.accent_soft = accents[cfg.accent].soft;
  clock24 = cfg.clock24;
  input_set_speed(speed_mickeys[cfg.speed]);
  wm_redraw_all();
  desk_clock_refresh();
}

static int find(const char *v, const char *const *names, int n, int def)
{
  int i;
  for (i = 0; i < n; i++)
    if (!strcasecmp(v, names[i]))
      return i;
  return def;
}

/* MDDESK.CFG next to the program (argv[0]) */
void settings_load(const char *argv0)
{
  char line[128], *eq, text[160];
  const char *slash;
  FILE *f;
  int i;

  for (i = 0; i < NBG; i++)
    bg_names[i] = backgrounds[i].name;
  for (i = 0; i < NACCENT; i++)
    accent_names[i] = accents[i].name;
  slash = argv0 ? strrchr(argv0, '\\') : 0;
  if (!slash && argv0)
    slash = strrchr(argv0, '/');
  if (slash && slash - argv0 < (int)sizeof(cfg_path) - 12)
  {
    memcpy(cfg_path, argv0, slash - argv0 + 1);
    strcpy(cfg_path + (slash - argv0 + 1), "MDDESK.CFG");
  }
  f = fopen(cfg_path, "r");
  while (f && fgets(line, sizeof(line), f))
  {
    line[strcspn(line, "\r\n")] = 0;
    if (!(eq = strchr(line, '=')))
      continue;
    *eq++ = 0;
    if (!strcasecmp(line, "background"))
      cfg.bg = find(eq, bg_names, NBG, cfg.bg);
    else if (!strcasecmp(line, "accent"))
      cfg.accent = find(eq, accent_names, NACCENT, cfg.accent);
    else if (!strcasecmp(line, "clock"))
      cfg.clock24 = atoi(eq) != 12;
    else if (!strcasecmp(line, "mouse"))
      cfg.speed = find(eq, speeds, NSPEED, cfg.speed);
  }
  if (f)
    fclose(f);
  apply();
  describe(text, sizeof(text));
  tlog("MDDESK: settings %s %s\n", f ? "loaded" : "default", text);
}

static void save(struct widget *w)
{
  FILE *f = fopen(cfg_path, "w");
  char text[160];

  (void)w;
  describe(text, sizeof(text));
  if (!f)
    return;
  fprintf(f, "background=%s\naccent=%s\nclock=%d\nmouse=%s\n", backgrounds[cfg.bg].name,
          accents[cfg.accent].name, cfg.clock24 ? 24 : 12, speeds[cfg.speed]);
  fclose(f);
  tlog("MDDESK: settings saved %s\n", text);
}

static void close_settings(void)
{
  if (win)
  {
    wm_window_close(win);
    win = 0;
    panel_damage();
  }
}

static void b_close(struct widget *w)
{
  (void)w;
  close_settings();
}

static void pick_bg(struct widget *w) { cfg.bg = w->value; apply(); }
static void pick_accent(struct widget *w) { cfg.accent = w->value; apply(); }
static void pick_speed(struct widget *w) { cfg.speed = w->value; apply(); }
static void toggle_clock(struct widget *w) { cfg.clock24 = w->value; apply(); }

static int events(struct window *w, const struct event *e)
{
  (void)w;
  if (e->type == EV_CLOSE)
  {
    close_settings();
    return 1;
  }
  return 0;
}

/* test log: where list item i is on the screen */
static void log_items(const char *prefix, struct widget *l, const char *const *names, int n)
{
  int i, x, y;

  if (!test_mode)
    return;
  ui_widget_center(l, &x, &y);
  for (i = 0; i < n; i++)
    tlog("MDDESK: widget %s_%s %d,%d\n", prefix, names[i], x, y - l->h / 2 + 2 + i * 20 + 10);
}

void settings_open(void)
{
  struct ui *ui;
  struct widget *bg, *ac, *sp, *b;

  if (win)
  {
    wm_raise(win);
    return;
  }
  win = wm_window_new(320, 120, 440, 380, "Settings");
  if (!win)
    return;
  win->handler = events;
  ui = ui_new(win);
  ui_label(ui, 16, 14, "Background");
  bg = ui_list(ui, 16, 36, 180, 108, bg_names, NBG, pick_bg);
  bg->value = cfg.bg;
  ui_label(ui, 220, 14, "Accent colour");
  ac = ui_list(ui, 220, 36, 196, 108, accent_names, NACCENT, pick_accent);
  ac->value = cfg.accent;
  ui_label(ui, 16, 162, "Mouse speed");
  sp = ui_list(ui, 16, 184, 180, 68, speeds, NSPEED, pick_speed);
  sp->value = cfg.speed;
  check24 = ui_check(ui, 220, 184, "24-hour clock", toggle_clock);
  check24->value = cfg.clock24;
  ui_label(ui, 220, 214, "Saved in MDDESK.CFG");
  b = ui_button(ui, 228, 300, 92, 28, "Save", save);
  desk_widget("settings_save", b);
  b = ui_button(ui, 328, 300, 92, 28, "Close", b_close);
  desk_widget("settings_close", b);
  desk_widget("settings_clock24", check24);
  log_items("bg", bg, bg_names, NBG);
  log_items("accent", ac, accent_names, NACCENT);
  ui_focus(bg);
  ui_redraw(ui);
  panel_damage();
  tlog("MDDESK: window Settings at %d,%d\n", win->x, win->y);
}
