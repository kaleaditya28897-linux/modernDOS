/* mdgui - text in the 8x16 code page 437 font (shared with the MDVMM
   console: mdvmm/core/font.c) */
#include <string.h>
#include "mdgui.h"

extern const uint8_t font8x16[4096];

/* draw text at x,y (top left); returns the width drawn */
int draw_text(struct surface *s, int x, int y, const char *text, color_t fg, color_t bg)
{
  int x0 = x, row, col;

  for (; *text; text++, x += FONT_W)
  {
    const uint8_t *glyph = font8x16 + (uint8_t)*text * FONT_H;

    if (x >= s->clip.x + s->clip.w || x + FONT_W <= s->clip.x)
      continue;
    for (row = 0; row < FONT_H; row++)
      for (col = 0; col < FONT_W; col++)
      {
        if (glyph[row] & (0x80 >> col))
          draw_pixel(s, x + col, y + row, fg);
        else if (bg != TEXT_TRANSPARENT)
          draw_pixel(s, x + col, y + row, bg);
      }
  }
  return x - x0;
}

int text_width(const char *text)
{
  return (int)strlen(text) * FONT_W;
}
