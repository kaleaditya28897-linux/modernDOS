/* MDVMM core - text screen on a UEFI framebuffer
 *
 * The virtual BIOS keeps the 80x25 text screen in RAM at B8000h (the
 * pages in vram_phys). On every timer tick the cells that changed since
 * the last update are drawn on the GOP framebuffer with an 8x16 font,
 * scaled up when the framebuffer has room, plus a blinking cursor.
 */
#include "core.h"

#define FB_VIRT  0xE0000000u
#define COLS     80
#define ROWS     25

extern const uint8_t font8x16[4096];

static uint32_t *fb;
static uint32_t pitch, scale, x0, y0;
static uint32_t palette[16];
static uint16_t shadow[COLS * ROWS];
static int cur_cell = -1, blink;
static uint32_t ticks;

static const uint8_t cga[16][3] = {
  { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0xaa }, { 0x00, 0xaa, 0x00 }, { 0x00, 0xaa, 0xaa },
  { 0xaa, 0x00, 0x00 }, { 0xaa, 0x00, 0xaa }, { 0xaa, 0x55, 0x00 }, { 0xaa, 0xaa, 0xaa },
  { 0x55, 0x55, 0x55 }, { 0x55, 0x55, 0xff }, { 0x55, 0xff, 0x55 }, { 0x55, 0xff, 0xff },
  { 0xff, 0x55, 0x55 }, { 0xff, 0x55, 0xff }, { 0xff, 0xff, 0x55 }, { 0xff, 0xff, 0xff },
};

void fbcon_init(void)
{
  uint32_t i, pages;

  if (!boot.fb_phys || boot.fb_width < COLS * 8 || boot.fb_height < ROWS * 16)
  {
    kprintf("fbcon: no usable framebuffer\n");
    return;
  }
  pages = (boot.fb_size + 4095) / 4096;
  for (i = 0; i < pages; i++)
    if (!map_page(FB_VIRT + i * 4096, boot.fb_phys + i * 4096, 3))
      return;
  fb = (uint32_t *)FB_VIRT;
  pitch = boot.fb_pitch;
  scale = boot.fb_width / (COLS * 8);
  if (boot.fb_height / (ROWS * 16) < scale)
    scale = boot.fb_height / (ROWS * 16);
  if (scale > 2)
    scale = 2;
  x0 = (boot.fb_width - COLS * 8 * scale) / 2;
  y0 = (boot.fb_height - ROWS * 16 * scale) / 2;
  for (i = 0; i < 16; i++)
    palette[i] = boot.fb_format == FB_RGBX
      ? (uint32_t)cga[i][0] | cga[i][1] << 8 | cga[i][2] << 16
      : (uint32_t)cga[i][2] | cga[i][1] << 8 | cga[i][0] << 16;
  for (i = 0; i < pitch * boot.fb_height; i++)
    fb[i] = 0;
  for (i = 0; i < COLS * ROWS; i++)
    shadow[i] = 0xffff;                 /* draw everything once */
  kprintf("fbcon: %ux%u framebuffer at %08x, scale %u\n", boot.fb_width,
          boot.fb_height, boot.fb_phys, scale);
}

static void draw_cell(int n, uint16_t v, int cursor)
{
  const uint8_t *glyph = font8x16 + (v & 0xff) * 16;
  uint32_t fg = palette[(v >> 8) & 15], bg = palette[(v >> 12) & 7];
  uint32_t x = x0 + (uint32_t)(n % COLS) * 8 * scale;
  uint32_t y = y0 + (uint32_t)(n / COLS) * 16 * scale;
  uint16_t shape = *(uint16_t *)rm_ptr(0x40, 0x60);
  int cs = shape >> 8 & 0x1f, ce = shape & 0x1f, row, col, sy, sx;

  if (shape & 0x2000)
    cursor = 0;                         /* hidden */
  if (ce < 8 && cs < 8)
  {
    cs *= 2;                            /* CGA style shape: 8 lines */
    ce = ce * 2 + 1;
  }
  for (row = 0; row < 16; row++)
  {
    uint8_t bits = glyph[row];
    if (cursor && row >= cs && row <= ce)
      bits = 0xff;
    for (sy = 0; sy < (int)scale; sy++)
    {
      uint32_t *p = fb + (y + row * scale + sy) * pitch + x;
      for (col = 0; col < 8; col++)
        for (sx = 0; sx < (int)scale; sx++)
          *p++ = bits & (0x80 >> col) ? fg : bg;
    }
  }
}

/* timer tick: draw what changed on the visible page */
void fbcon_update(void)
{
  const uint16_t *page;
  uint16_t pos;
  int n, c;
  uint8_t active;

  if (!fb)
    return;
  active = *(uint8_t *)rm_ptr(0x40, 0x62) & 7;
  page = pool_ptr(vram_phys[active]);   /* 4K per text page */
  pos = *(uint16_t *)rm_ptr(0x40, 0x50 + active * 2);
  c = (pos >> 8) * COLS + (pos & 0xff);
  if ((pos >> 8) >= ROWS || (pos & 0xff) >= COLS)
    c = -1;
  if (++ticks % 4 == 0)
    blink = !blink;

  for (n = 0; n < COLS * ROWS; n++)
  {
    int cursor = n == c && blink;
    if (page[n] != shadow[n] || n == cur_cell || n == c)
    {
      draw_cell(n, page[n], cursor);
      shadow[n] = page[n];
    }
  }
  cur_cell = c;
}
