/*
 * GFXTEST - mdgui graphics test (DJGPP).
 *
 * Opens a 32 bpp linear framebuffer mode (1024x768 if there is one),
 * draws a known pattern and logs "GFXTEST: drawn WxH" to port E9h; the
 * harness then takes a screenshot and checks pixels (tests/gui/gfxcheck.py):
 *   - 120x120 boxes in the corners: red, green, blue, white (top left,
 *     top right, bottom left, bottom right)
 *   - background RGB(32,48,64)
 *   - a gradient bar and text in the middle
 * After 3 seconds it returns to text mode; exit code 0 if all went well.
 */
#include <stdio.h>
#include <string.h>
#include <pc.h>
#include <go32.h>
#include <sys/farptr.h>
#include "mdgui.h"

static void out(const char *s)
{
  for (; *s; s++)
  {
    if (*s == '\n')
      outportb(0xe9, '\r');
    outportb(0xe9, *s);
  }
}

static unsigned long ticks(void)
{
  return _farpeekl(_dos_ds, 0x46c);
}

int main(void)
{
  struct surface *s;
  const struct gfx_mode *m;
  char buf[128];
  unsigned long t;
  int w, h;

  out("\ngraphics test\n");
  if (gfx_open(1024, 768))
  {
    sprintf(buf, "GFXTEST: failed: %s\n", gfx_error());
    out(buf);
    return 1;
  }
  m = gfx_mode();
  s = gfx_screen();
  w = m->w;
  h = m->h;

  draw_fill(s, 0, 0, w, h, RGB(32, 48, 64));
  draw_fill(s, 0, 0, 120, 120, RGB(255, 0, 0));
  draw_fill(s, w - 120, 0, 120, 120, RGB(0, 255, 0));
  draw_fill(s, 0, h - 120, 120, 120, RGB(0, 0, 255));
  draw_fill(s, w - 120, h - 120, 120, 120, RGB(255, 255, 255));
  draw_gradient(s, w / 2 - 200, h / 2 - 60, 400, 40, RGB(40, 80, 200), RGB(200, 230, 255));
  draw_frame(s, w / 2 - 201, h / 2 - 61, 402, 42, RGB(255, 255, 255));
  draw_text(s, w / 2 - text_width("modernDOS graphics") / 2, h / 2 + 10,
            "modernDOS graphics", RGB(255, 220, 80), TEXT_TRANSPARENT);
  draw_line(s, 120, 120, w - 120, h - 120, RGB(255, 128, 0));
  /* clipping: this must not touch the top left box */
  surface_clip(s, 130, 0, w - 130, h);
  draw_fill(s, 0, 0, 125, 125, RGB(255, 0, 255));
  surface_noclip(s);
  gfx_present_all();

  sprintf(buf, "GFXTEST: drawn %dx%d, VBE mode %x, framebuffer %08lx\n", w, h, m->vbe_mode,
          (unsigned long)m->phys);
  out(buf);
  for (t = ticks(); ticks() - t < 55;)  /* 3 s for the screenshot */
    ;
  gfx_close();
  printf("Back in text mode after graphics.\n");
  out("GFXTEST: text mode again\n");
  return 0;
}
