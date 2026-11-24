/* MDVMM core - text screen on a UEFI framebuffer
 *
 * The virtual BIOS keeps the 80x25 text screen in RAM at B8000h (the
 * pages in vram_phys). On every timer tick the cells that changed since
 * the last update are drawn on the GOP framebuffer with an 8x16 font,
 * scaled up when the framebuffer has room, plus a blinking cursor.
 *
 * Before DOS starts, and after a panic, the framebuffer shows the
 * monitor's own log (kprintf) instead: on real machines there is no other
 * way to see why a boot stopped.
 */
#include "core.h"

#define COLS     80
#define ROWS     25

extern const uint8_t font8x16[4096];

static uint32_t *fb;
static uint32_t pitch, scale, x0, y0;
static uint32_t palette[16];
static uint16_t shadow[COLS * ROWS];
static int cur_cell = -1, blink;
static int suspended;                   /* a program owns the framebuffer */
static int mouse_cell = -1;             /* where the mouse cursor was drawn */
static uint32_t ticks;

/* the monitor's log: the last lines are kept, and drawn while log_on */
#define LOG_LINES 64
#define LOG_COLS  120
static char ring[LOG_LINES][LOG_COLS + 1];
static uint32_t ring_line, ring_col;    /* line being written (not modulo) */
static int log_on;
static uint32_t lcols, lrows, lrow, lcol, lscale;

static const uint8_t cga[16][3] = {
  { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0xaa }, { 0x00, 0xaa, 0x00 }, { 0x00, 0xaa, 0xaa },
  { 0xaa, 0x00, 0x00 }, { 0xaa, 0x00, 0xaa }, { 0xaa, 0x55, 0x00 }, { 0xaa, 0xaa, 0xaa },
  { 0x55, 0x55, 0x55 }, { 0x55, 0x55, 0xff }, { 0x55, 0xff, 0x55 }, { 0x55, 0xff, 0xff },
  { 0xff, 0x55, 0x55 }, { 0xff, 0x55, 0xff }, { 0xff, 0xff, 0x55 }, { 0xff, 0xff, 0xff },
};

static void log_replay(uint32_t lines);

void fbcon_init(void)
{
  uint32_t i, pages;

  if (!boot.fb_phys || boot.fb_width < COLS * 8 || boot.fb_height < ROWS * 16)
  {
    kprintf("fbcon: no usable framebuffer\n");
    return;
  }
  if (boot.fb_size > FB_MAX)
    boot.fb_size = FB_MAX;
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
  lscale = boot.fb_width >= 1600 && boot.fb_height >= 900 ? 2 : 1;  /* readable on a photo */
  lcols = boot.fb_width / (8 * lscale);
  if (lcols > LOG_COLS)
    lcols = LOG_COLS;
  lrows = boot.fb_height / (16 * lscale);
  log_on = 1;
  log_replay(lrows - 1);                /* what was logged before this */
  kprintf("fbcon: %ux%u framebuffer at %08x, pitch %u, scale %u\n", boot.fb_width,
          boot.fb_height, boot.fb_phys, pitch, scale);
}

/* the log ends, the DOS text screen begins */
void fbcon_text(void)
{
  int i;

  if (!fb)
    return;
  log_on = 0;
  fbcon_clear();
  for (i = 0; i < COLS * ROWS; i++)
    shadow[i] = 0xffff;
  cur_cell = -1;
}

/* a panic: back to the log, with its last lines */
void fbcon_panic(void)
{
  if (!fb || log_on)
    return;
  log_on = 1;
  log_replay(lrows > 24 ? lrows - 12 : lrows - 1);
}

static void log_glyph(uint32_t row, uint32_t col, uint8_t ch)
{
  const uint8_t *glyph = font8x16 + ch * 16;
  uint32_t y, x, sy, sx;

  for (y = 0; y < 16; y++)
    for (sy = 0; sy < lscale; sy++)
    {
      uint32_t *p = fb + ((row * 16 + y) * lscale + sy) * pitch + col * 8 * lscale;
      for (x = 0; x < 8; x++)
        for (sx = 0; sx < lscale; sx++)
          *p++ = glyph[y] & (0x80 >> x) ? palette[7] : 0;
    }
}

/* clear, then draw the last `lines` lines of the log */
static void log_replay(uint32_t lines)
{
  uint32_t first, l, c;

  fbcon_clear();
  first = ring_line + 1 > lines ? ring_line + 1 - lines : 0;
  if (ring_line >= LOG_LINES && first < ring_line - (LOG_LINES - 1))
    first = ring_line - (LOG_LINES - 1);
  for (l = first; l <= ring_line; l++)
    for (c = 0; c < lcols && ring[l % LOG_LINES][c]; c++)
      log_glyph(l - first, c, (uint8_t)ring[l % LOG_LINES][c]);
  lrow = ring_line - first;
  lcol = ring_col < lcols ? ring_col : lcols;
}

/* one character of the monitor's log (from kprintf) */
void fbcon_log(char c)
{
  char *line = ring[ring_line % LOG_LINES];

  if (c == '\r')
    return;
  if (c == '\n')
  {
    ring_line++;
    ring_col = 0;
    ring[ring_line % LOG_LINES][0] = 0;
    if (log_on && fb && ++lrow >= lrows)
      log_replay(lrows / 2);            /* full: keep the newer half */
    lcol = 0;
    return;
  }
  if (ring_col < LOG_COLS)
  {
    line[ring_col++] = c;
    line[ring_col] = 0;
  }
  if (log_on && fb && lcol < lcols)
    log_glyph(lrow, lcol++, (uint8_t)c);
}

/* the last `lines` lines of the log (0: all that are kept) as text */
uint32_t fbcon_log_copy(char *dst, uint32_t room, uint32_t lines)
{
  uint32_t first = ring_line >= LOG_LINES ? ring_line - (LOG_LINES - 1) : 0, l, n = 0;
  const char *s;

  if (lines && ring_line + 1 > lines && ring_line + 1 - lines > first)
    first = ring_line + 1 - lines;
  for (l = first; l <= ring_line; l++)
  {
    for (s = ring[l % LOG_LINES]; *s && n + 2 < room; s++)
      dst[n++] = *s;
    if (n + 2 < room && l < ring_line)
    {
      dst[n++] = '\r';
      dst[n++] = '\n';
    }
  }
  return n;
}

/* clear the whole framebuffer */
void fbcon_clear(void)
{
  uint32_t i;

  if (fb)
    for (i = 0; i < pitch * boot.fb_height; i++)
      fb[i] = 0;
}

/* a VBE graphics mode starts (on) or ends: stop or resume drawing text */
void fbcon_graphics(int on)
{
  int i;

  suspended = on;
  if (!on)
  {
    fbcon_clear();
    for (i = 0; i < COLS * ROWS; i++)
      shadow[i] = 0xffff;               /* redraw everything */
    cur_cell = -1;
    mouse_cell = -1;
  }
}

static void draw_cell(int n, uint16_t v, int cursor, int invert)
{
  const uint8_t *glyph = font8x16 + (v & 0xff) * 16;
  uint32_t fg = palette[(v >> 8) & 15], bg = palette[(v >> 12) & 7];

  if (invert)                           /* the mouse cursor */
  {
    fg = palette[((v >> 12) & 7) ^ 15];
    bg = palette[((v >> 8) & 7) ^ 7];
  }
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
  int n, c, mc;
  uint8_t active;

  if (!fb || suspended || log_on)
    return;
  active = *(uint8_t *)rm_ptr(0x40, 0x62) & 7;
  page = pool_ptr(vram_phys[active]);   /* 4K per text page */
  pos = *(uint16_t *)rm_ptr(0x40, 0x50 + active * 2);
  c = (pos >> 8) * COLS + (pos & 0xff);
  if ((pos >> 8) >= ROWS || (pos & 0xff) >= COLS)
    c = -1;
  if (++ticks % 4 == 0)
    blink = !blink;

  mc = mouse_text_cell();
  for (n = 0; n < COLS * ROWS; n++)
  {
    int cursor = n == c && blink;
    if (page[n] != shadow[n] || n == cur_cell || n == c ||
        (mc != mouse_cell && (n == mc || n == mouse_cell)))
    {
      draw_cell(n, page[n], cursor, n == mc);
      shadow[n] = page[n];
    }
  }
  mouse_cell = mc;
  cur_cell = c;
}
