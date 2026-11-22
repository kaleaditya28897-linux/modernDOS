/* mdgui - surfaces and drawing primitives, clipped to surface->clip */
#include <stdlib.h>
#include <string.h>
#include "mdgui.h"

struct surface *surface_new(int w, int h)
{
  struct surface *s = malloc(sizeof(*s));

  if (!s)
    return 0;
  s->px = calloc((size_t)w * h, sizeof(color_t));
  if (!s->px)
  {
    free(s);
    return 0;
  }
  s->w = w;
  s->h = h;
  s->stride = w;
  surface_noclip(s);
  return s;
}

void surface_free(struct surface *s)
{
  if (!s)
    return;
  free(s->px);
  free(s);
}

void surface_noclip(struct surface *s)
{
  s->clip.x = s->clip.y = 0;
  s->clip.w = s->w;
  s->clip.h = s->h;
}

/* clip to x,y,w,h (within the surface) */
void surface_clip(struct surface *s, int x, int y, int w, int h)
{
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > s->w) w = s->w - x;
  if (y + h > s->h) h = s->h - y;
  s->clip.x = x;
  s->clip.y = y;
  s->clip.w = w > 0 ? w : 0;
  s->clip.h = h > 0 ? h : 0;
}

/* intersect x,y,w,h with the clip rectangle; 0 if nothing is left */
static int clip(const struct surface *s, int *x, int *y, int *w, int *h)
{
  int x1 = *x + *w, y1 = *y + *h;
  int cx1 = s->clip.x + s->clip.w, cy1 = s->clip.y + s->clip.h;

  if (*x < s->clip.x) *x = s->clip.x;
  if (*y < s->clip.y) *y = s->clip.y;
  if (x1 > cx1) x1 = cx1;
  if (y1 > cy1) y1 = cy1;
  *w = x1 - *x;
  *h = y1 - *y;
  return *w > 0 && *h > 0;
}

void draw_pixel(struct surface *s, int x, int y, color_t c)
{
  if (x >= s->clip.x && y >= s->clip.y && x < s->clip.x + s->clip.w && y < s->clip.y + s->clip.h)
    s->px[y * s->stride + x] = c;
}

void draw_fill(struct surface *s, int x, int y, int w, int h, color_t c)
{
  int i, j;

  if (!clip(s, &x, &y, &w, &h))
    return;
  for (j = y; j < y + h; j++)
  {
    color_t *p = s->px + j * s->stride + x;
    for (i = 0; i < w; i++)
      p[i] = c;
  }
}

void draw_hline(struct surface *s, int x, int y, int w, color_t c)
{
  draw_fill(s, x, y, w, 1, c);
}

void draw_vline(struct surface *s, int x, int y, int h, color_t c)
{
  draw_fill(s, x, y, 1, h, c);
}

void draw_frame(struct surface *s, int x, int y, int w, int h, color_t c)
{
  if (w <= 0 || h <= 0)
    return;
  draw_hline(s, x, y, w, c);
  draw_hline(s, x, y + h - 1, w, c);
  draw_vline(s, x, y + 1, h - 2, c);
  draw_vline(s, x + w - 1, y + 1, h - 2, c);
}

void draw_line(struct surface *s, int x0, int y0, int x1, int y1, color_t c)
{
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;

  for (;;)
  {
    int e2 = 2 * err;

    draw_pixel(s, x0, y0, c);
    if (x0 == x1 && y0 == y1)
      break;
    if (e2 >= dy)
    {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx)
    {
      err += dx;
      y0 += sy;
    }
  }
}

static color_t mix(color_t a, color_t b, int t, int n)
{
  int r = (int)(a >> 16 & 0xff) + ((int)(b >> 16 & 0xff) - (int)(a >> 16 & 0xff)) * t / n;
  int g = (int)(a >> 8 & 0xff) + ((int)(b >> 8 & 0xff) - (int)(a >> 8 & 0xff)) * t / n;
  int bl = (int)(a & 0xff) + ((int)(b & 0xff) - (int)(a & 0xff)) * t / n;
  return RGB(r, g, bl);
}

/* vertical gradient from top to bottom */
void draw_gradient(struct surface *s, int x, int y, int w, int h, color_t top, color_t bottom)
{
  int j;

  for (j = 0; j < h; j++)
    draw_hline(s, x, y + j, w, mix(top, bottom, j, h > 1 ? h - 1 : 1));
}

void draw_blit(struct surface *dst, int dx, int dy, const struct surface *src,
               int sx, int sy, int w, int h)
{
  int x = dx, y = dy, j;

  if (!clip(dst, &x, &y, &w, &h))
    return;
  sx += x - dx;
  sy += y - dy;
  if (sx < 0 || sy < 0 || sx + w > src->w || sy + h > src->h)
    return;
  for (j = 0; j < h; j++)
    memmove(dst->px + (y + j) * dst->stride + x, src->px + (sy + j) * src->stride + sx,
            w * sizeof(color_t));
}
