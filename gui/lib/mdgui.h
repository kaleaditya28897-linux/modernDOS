/* mdgui - modernDOS graphics library (32-bit DPMI, DJGPP)
 *
 * Graphics layer of the modernDOS desktop: a VBE linear framebuffer mode
 * with 32 bits per pixel, an off-screen back buffer the program draws in,
 * and gfx_present() to copy changed areas to the screen. Colors are
 * 0x00RRGGBB everywhere; the conversion to the mode's pixel layout
 * happens when presenting.
 */
#ifndef MDGUI_H
#define MDGUI_H

#include <stdint.h>

typedef uint32_t color_t;

#define RGB(r, g, b)  ((color_t)(((r) & 0xff) << 16 | ((g) & 0xff) << 8 | ((b) & 0xff)))

struct rect {
  int x, y, w, h;
};

/* a drawing target: pixels, size, and a clip rectangle all drawing is
   limited to (initially the whole surface) */
struct surface {
  int w, h, stride;                     /* stride in pixels */
  color_t *px;
  struct rect clip;
};

/* the display */
struct gfx_mode {
  int w, h;
  int vbe_mode;
  uint32_t phys;                        /* framebuffer address */
};

/* gfx.c - display */
int gfx_open(int want_w, int want_h);   /* 0 = ok, else an error message */
const char *gfx_error(void);
void gfx_close(void);                   /* back to 80x25 text mode */
const struct gfx_mode *gfx_mode(void);
struct surface *gfx_screen(void);       /* the back buffer */
void gfx_present(int x, int y, int w, int h);
void gfx_present_all(void);
/* mouse cursor, drawn over the presented screen (not in the back buffer) */
void gfx_cursor_show(int on);
void gfx_cursor_move(int x, int y);

/* draw.c - drawing on surfaces (clipped) */
struct surface *surface_new(int w, int h);
void surface_free(struct surface *s);
void surface_clip(struct surface *s, int x, int y, int w, int h);
void surface_noclip(struct surface *s);
void draw_pixel(struct surface *s, int x, int y, color_t c);
void draw_fill(struct surface *s, int x, int y, int w, int h, color_t c);
void draw_hline(struct surface *s, int x, int y, int w, color_t c);
void draw_vline(struct surface *s, int x, int y, int h, color_t c);
void draw_frame(struct surface *s, int x, int y, int w, int h, color_t c);
void draw_line(struct surface *s, int x0, int y0, int x1, int y1, color_t c);
void draw_gradient(struct surface *s, int x, int y, int w, int h, color_t top, color_t bottom);
void draw_blit(struct surface *dst, int dx, int dy, const struct surface *src,
               int sx, int sy, int w, int h);

/* text.c - 8x16 bitmap font (code page 437) */
#define FONT_W 8
#define FONT_H 16
#define TEXT_TRANSPARENT 0xff000000u    /* as bg: leave the background */
int draw_text(struct surface *s, int x, int y, const char *text, color_t fg, color_t bg);
int text_width(const char *text);

/* input.c - keyboard (INT 16h) and mouse (INT 33h) events */
enum { EV_NONE, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_KEY };
#define MB_LEFT   1
#define MB_RIGHT  2
#define MB_MIDDLE 4

struct event {
  int type;
  int x, y;                             /* mouse position (all events) */
  int buttons;                          /* buttons held (MB_*) */
  int button;                           /* EV_MOUSE_DOWN/UP: which one */
  int scan, ascii;                      /* EV_KEY: BIOS scan code, character */
  int shift;                            /* EV_KEY: BIOS shift flags */
};

int input_open(void);                   /* 1 if there is a mouse */
int input_poll(struct event *e);        /* 1 if *e is an event */
void input_wait(struct event *e);       /* until there is one; idles */
void input_close(void);

/* wm.c - windows: frames, z-order, focus, moving, damage repaint */
#define WM_TITLE_H 24
#define WM_BORDER  1

struct window;
/* event handler: mouse coordinates are client coordinates; return 1 if
   handled. EV_CLOSE when the close button is clicked (default: close) */
enum { EV_CLOSE = 100 };
typedef int (*wm_handler)(struct window *w, const struct event *e);

struct window {
  int x, y, w, h;                       /* outer rectangle, screen coords */
  char title[64];
  struct surface *client;               /* contents, drawn by the program */
  wm_handler handler;
  void *user;
  int visible;
  int popup;                            /* no title bar; closes when the
                                           mouse is pressed outside it */
};

int wm_init(void);                      /* after gfx_open; draws the desktop */
struct window *wm_window_new(int x, int y, int w, int h, const char *title);
struct window *wm_popup_new(int x, int y, int w, int h);
void wm_window_close(struct window *w);
void wm_window_move(struct window *w, int x, int y);
void wm_raise(struct window *w);        /* to the top, with the focus */
struct window *wm_focus(void);
struct window *wm_top(void);
int wm_count(void);
struct window *wm_window(int i);        /* bottom (0) to top */
void wm_invalidate(struct window *w);   /* client contents changed */
void wm_damage(int x, int y, int w, int h);
void wm_update(void);                   /* repaint and present damage */
int wm_dispatch(const struct event *e); /* route one event; 0 if unused */
void wm_run(int (*idle)(void));         /* until wm_quit; idle may be 0 */
void wm_quit(void);

/* ui.c - widgets in a window */
enum { UI_LABEL, UI_BUTTON, UI_CHECK, UI_TEXT, UI_LIST, UI_MENUBAR };

struct ui;
struct widget;
typedef void (*ui_action)(struct widget *w);

struct ui_menu {
  const char *title;
  const char *const *items;
  int nitems;
};

struct widget {
  int type;
  int x, y, w, h;                       /* client coordinates */
  char text[128];                       /* caption; UI_TEXT: the contents */
  int value;                            /* check: on; list: selection (-1);
                                           text: caret; menubar: item chosen */
  int menu;                             /* menubar: menu chosen */
  int scroll;                           /* list: first row; text: first char */
  const char *const *items;             /* list items */
  int nitems;
  const struct ui_menu *menus;          /* menubar */
  int nmenus;
  int disabled;
  ui_action action;                     /* click, toggle, select, Enter, menu */
  void *user;
  struct ui *ui;
};

struct ui *ui_new(struct window *win);  /* takes the window's handler; the
                                           old one gets unused events */
struct widget *ui_label(struct ui *ui, int x, int y, const char *text);
struct widget *ui_button(struct ui *ui, int x, int y, int w, int h, const char *text, ui_action a);
struct widget *ui_check(struct ui *ui, int x, int y, const char *text, ui_action a);
struct widget *ui_textfield(struct ui *ui, int x, int y, int w, ui_action a);
struct widget *ui_list(struct ui *ui, int x, int y, int w, int h, const char *const *items, int n,
                       ui_action a);
struct widget *ui_menubar(struct ui *ui, const struct ui_menu *menus, int n, ui_action a);
void ui_set_text(struct widget *w, const char *text);
void ui_focus(struct widget *w);
struct widget *ui_focused(struct ui *ui);
void ui_redraw(struct ui *ui);
int ui_widget_center(const struct widget *w, int *sx, int *sy);   /* screen */

#endif
