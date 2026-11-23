/* mdgui - display: VBE linear framebuffer modes, back buffer, present
 *
 * Uses the VBE BIOS through DPMI (real BIOS on DOS boots, the MDVMM
 * virtual BIOS on UEFI boots). Only 32 bpp direct color modes with a
 * linear framebuffer are used; the framebuffer is mapped with DPMI 0800h
 * and reached through its own selector.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include "mdgui.h"

struct vbe_mode_info {
  uint16_t attr;
  uint8_t win_a, win_b;
  uint16_t gran, win_size, seg_a, seg_b;
  uint32_t win_func;
  uint16_t pitch, w, h;
  uint8_t cw, ch, planes, bpp, banks, model, bank_size, pages, res1;
  uint8_t r_size, r_pos, g_size, g_pos, b_size, b_pos, x_size, x_pos, dc_info;
  uint32_t phys, res2;
  uint16_t res3, lin_pitch;
  uint8_t bnk_pages, lin_pages;
  uint8_t lr_size, lr_pos, lg_size, lg_pos, lb_size, lb_pos, lx_size, lx_pos;
  uint32_t max_clock;
  uint8_t pad[190];
} __attribute__((packed));

static struct gfx_mode mode;
static struct surface *screen;
static int sel = -1;                    /* framebuffer selector */
static uint32_t pitch;                  /* bytes */
static int r_pos, g_pos, b_pos;
static uint32_t *row;                   /* conversion buffer, one line */
static char error[96];

/* mouse cursor: arrow sprite, '#' outline, 'o' fill */
#define CUR_W 11
#define CUR_H 17
static const char *const cursor_shape[CUR_H] = {
  "#          ", "##         ", "#o#        ", "#oo#       ", "#ooo#      ",
  "#oooo#     ", "#ooooo#    ", "#oooooo#   ", "#ooooooo#  ", "#oooooooo# ",
  "#ooooooooo#", "#oooooo####", "#ooo#oo#   ", "#oo# #oo#  ", "#o#  #oo#  ",
  "##    #oo# ", "      ###  ",
};
static int cur_on, cur_x, cur_y;
static __dpmi_meminfo map;

const char *gfx_error(void)
{
  return error;
}

const struct gfx_mode *gfx_mode(void)
{
  return &mode;
}

struct surface *gfx_screen(void)
{
  return screen;
}

static int vbe(__dpmi_regs *r)
{
  __dpmi_int(0x10, r);
  return r->x.ax == 0x004f;
}

static int mode_info(int m, struct vbe_mode_info *mi)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  r.x.ax = 0x4f01;
  r.x.cx = (uint16_t)m;
  r.x.es = __tb >> 4;
  r.x.di = __tb & 15;
  if (!vbe(&r))
    return 0;
  dosmemget(__tb, sizeof(*mi), mi);
  return 1;
}

static int usable(const struct vbe_mode_info *mi)
{
  return (mi->attr & 0x91) == 0x91 && mi->bpp == 32 && mi->model == 6 && mi->phys &&
         mi->r_size == 8 && mi->g_size == 8 && mi->b_size == 8;
}

/* the mode to use: want_w x want_h if there is one, else the largest
   that fits in it, else the smallest */
static int pick(int want_w, int want_h, struct vbe_mode_info *best)
{
  __dpmi_regs r;
  uint8_t info[512];
  uint16_t modes[256];
  uint32_t list;
  int best_mode = -1, i;

  memset(best, 0, sizeof(*best));
  memset(&r, 0, sizeof(r));
  memset(info, 0, sizeof(info));
  memcpy(info, "VBE2", 4);
  dosmemput(info, sizeof(info), __tb);
  r.x.ax = 0x4f00;
  r.x.es = __tb >> 4;
  r.x.di = __tb & 15;
  if (!vbe(&r))
  {
    strcpy(error, "no VBE BIOS");
    return -1;
  }
  dosmemget(__tb, sizeof(info), info);
  if (memcmp(info, "VESA", 4) || *(uint16_t *)(info + 4) < 0x0200)
  {
    strcpy(error, "VBE 2.0 or later needed");
    return -1;
  }
  /* copy the mode list first: it may be in the buffer (__tb) that the
     mode information calls overwrite */
  list = *(uint32_t *)(info + 14);
  list = (list >> 16) * 16 + (list & 0xffff);
  dosmemget(list, sizeof(modes), modes);
  modes[255] = 0xffff;
  for (i = 0; i < 256; i++)
  {
    uint16_t m = modes[i];
    struct vbe_mode_info mi;
    int fits, best_fits;

    if (m == 0xffff)
      break;
    if (!mode_info(m, &mi) || !usable(&mi))
      continue;
    if (mi.w == want_w && mi.h == want_h)
    {
      *best = mi;
      return m;
    }
    fits = mi.w <= want_w && mi.h <= want_h;
    best_fits = best_mode >= 0 && best->w <= want_w && best->h <= want_h;
    if (best_mode < 0 ||
        (fits && (!best_fits || mi.w * mi.h > best->w * best->h)) ||
        (!fits && !best_fits && mi.w * mi.h < best->w * best->h))
    {
      *best = mi;
      best_mode = m;
    }
  }
  if (best_mode < 0)
    strcpy(error, "no 32 bpp linear framebuffer mode");
  return best_mode;
}

int gfx_open(int want_w, int want_h)
{
  struct vbe_mode_info mi;
  __dpmi_regs r;
  int m = pick(want_w, want_h, &mi);

  if (m < 0)
    return -1;
  pitch = mi.lin_pitch ? mi.lin_pitch : mi.pitch;
  r_pos = mi.lr_size ? mi.lr_pos : mi.r_pos;
  g_pos = mi.lg_size ? mi.lg_pos : mi.g_pos;
  b_pos = mi.lb_size ? mi.lb_pos : mi.b_pos;

  /* map the framebuffer, give it a selector */
  map.address = mi.phys;
  map.size = (pitch * mi.h + 4095) & ~4095u;
  if (__dpmi_physical_address_mapping(&map))
  {
    strcpy(error, "cannot map the framebuffer");
    return -1;
  }
  sel = __dpmi_allocate_ldt_descriptors(1);
  if (sel < 0 || __dpmi_set_segment_base_address(sel, map.address) ||
      __dpmi_set_segment_limit(sel, map.size - 1))
  {
    strcpy(error, "cannot make a framebuffer selector");
    return -1;
  }
  screen = surface_new(mi.w, mi.h);
  row = malloc(mi.w * 4);
  if (!screen || !row)
  {
    strcpy(error, "out of memory");
    return -1;
  }

  memset(&r, 0, sizeof(r));
  r.x.ax = 0x4f02;
  r.x.bx = (uint16_t)(m | 0x4000);      /* linear framebuffer */
  if (!vbe(&r))
  {
    sprintf(error, "cannot set mode %x", m);
    return -1;
  }
  mode.w = mi.w;
  mode.h = mi.h;
  mode.vbe_mode = m;
  mode.phys = mi.phys;
  return 0;
}

void gfx_close(void)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  r.x.ax = 0x0003;
  __dpmi_int(0x10, &r);
  if (sel >= 0)
    __dpmi_free_ldt_descriptor(sel);
  if (map.address)
    __dpmi_free_physical_address_mapping(&map);
  sel = -1;
  map.address = 0;
  surface_free(screen);
  screen = 0;
  cur_on = 0;
  free(row);
  row = 0;
}

void gfx_present(int x, int y, int w, int h)
{
  int j, i;

  if (!screen || sel < 0)
    return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > screen->w) w = screen->w - x;
  if (y + h > screen->h) h = screen->h - y;
  if (w <= 0 || h <= 0)
    return;
  for (j = y; j < y + h; j++)
  {
    const color_t *src = screen->px + j * screen->stride + x;
    int cy = j - cur_y;
    int on_cursor = cur_on && cy >= 0 && cy < CUR_H && cur_x < x + w && cur_x + CUR_W > x;

    if (r_pos == 16 && g_pos == 8 && b_pos == 0 && !on_cursor)
    {
      movedata(_my_ds(), (unsigned)src, sel, j * pitch + x * 4, w * 4);
      continue;
    }
    memcpy(row, src, w * 4);
    if (on_cursor)                      /* the cursor goes over the screen */
      for (i = 0; i < CUR_W; i++)
      {
        int px = cur_x + i - x;
        char c = cursor_shape[cy][i];
        if (px >= 0 && px < w && c != ' ')
          row[px] = c == '#' ? RGB(16, 16, 16) : RGB(255, 255, 255);
      }
    if (!(r_pos == 16 && g_pos == 8 && b_pos == 0))
      for (i = 0; i < w; i++)
        row[i] = (row[i] >> 16 & 0xff) << r_pos | (row[i] >> 8 & 0xff) << g_pos |
                 (row[i] & 0xff) << b_pos;
    movedata(_my_ds(), (unsigned)row, sel, j * pitch + x * 4, w * 4);
  }
}

void gfx_cursor_show(int on)
{
  cur_on = on;
  gfx_present(cur_x, cur_y, CUR_W, CUR_H);
}

void gfx_cursor_move(int x, int y)
{
  int ox = cur_x, oy = cur_y;

  if (x == cur_x && y == cur_y)
    return;
  cur_x = x;
  cur_y = y;
  gfx_present(ox, oy, CUR_W, CUR_H);    /* uncover the old place */
  gfx_present(x, y, CUR_W, CUR_H);
}

void gfx_present_all(void)
{
  if (screen)
    gfx_present(0, 0, screen->w, screen->h);
}
