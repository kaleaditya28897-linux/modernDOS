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

#endif
