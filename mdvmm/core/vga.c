/* MDVMM core - VGA mode 13h on the UEFI framebuffer
 *
 * A UEFI machine has no video BIOS to set VGA modes, and often no VGA
 * hardware behind the legacy ports and A0000h either. For the mode DOS
 * games use - 13h, 320x200 in 256 colours, one byte per pixel at A000:0 -
 * the monitor is the display card:
 *  - INT 10h AH=00h AL=13h puts 64 KB of memory at A0000h
 *  - the palette (DAC, ports 3C6h-3C9h) and the status port 3DAh are the
 *    monitor's: V86 programs through the I/O bitmap; DPMI clients, which
 *    run with I/O privilege, through I/O breakpoints in the debug
 *    registers (the access is seen after it happened: a write is noted, a
 *    read gets its value put into AL afterwards)
 *  - INT 10h palette and pixel functions
 *  - about 30 times a second the picture is drawn on the framebuffer,
 *    enlarged by a whole factor and centred
 * Not here: the planar modes (0Dh-12h, "mode X") and text in mode 13h.
 * One program at a time has the mode: the one on the display.
 */
#include "core.h"

#define VGA_LIN   0xA0000u
#define VGA_PAGES 16
#define W 320
#define H 200

static int active, owner;
static uint32_t pages[VGA_PAGES];
static uint8_t dac[256][3];             /* 6 bit components */
static uint32_t pal[256];               /* as framebuffer pixels */
static int widx, wsub, ridx, rsub;
static uint8_t wcomp[3];
static uint32_t status_reads, last_ms;
static int pal_dirty;
static int dac_reading;                 /* 3C7h was written last, not 3C8h */

unsigned long mdvmm_ms(void);

static void set_dac(int i, uint8_t r, uint8_t g, uint8_t b)
{
  uint32_t r8, g8, b8;

  i &= 255;
  dac[i][0] = r & 63;
  dac[i][1] = g & 63;
  dac[i][2] = b & 63;
  r8 = (uint32_t)(dac[i][0] << 2 | dac[i][0] >> 4);
  g8 = (uint32_t)(dac[i][1] << 2 | dac[i][1] >> 4);
  b8 = (uint32_t)(dac[i][2] << 2 | dac[i][2] >> 4);
  pal[i] = boot.fb_format == FB_RGBX ? r8 | g8 << 8 | b8 << 16 : b8 | g8 << 8 | r8 << 16;
  pal_dirty = 1;
}

/* the palette a VGA BIOS sets for mode 13h: 16 EGA colours, 16 greys,
   then a colour circle in three brightnesses (close to the original) */
static void default_palette(void)
{
  static const uint8_t ega[16][3] = {
    { 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 }, { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 },
    { 42, 42, 42 }, { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 }, { 63, 21, 21 },
    { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 } };
  static const uint8_t grey[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
  static const uint8_t top[3] = { 63, 28, 16 }, low[3][3] = { { 0, 31, 45 }, { 0, 14, 20 }, { 0, 8, 11 } };
  int i, level, sat, hue;

  for (i = 0; i < 16; i++)
    set_dac(i, ega[i][0], ega[i][1], ega[i][2]);
  for (i = 0; i < 16; i++)
    set_dac(16 + i, grey[i], grey[i], grey[i]);
  i = 32;
  for (level = 0; level < 3; level++)
    for (sat = 0; sat < 3; sat++)
      for (hue = 0; hue < 24; hue++)
      {
        /* around the circle: blue, magenta, red, yellow, green, cyan */
        uint32_t hi = top[level], lo = low[sat][level], seg = (uint32_t)hue / 4, step = (uint32_t)hue % 4;
        uint32_t up = lo + (hi - lo) * step / 4, down = hi - (hi - lo) * step / 4, r, g, b;
        switch (seg)
        {
          case 0: r = up; g = lo; b = hi; break;
          case 1: r = hi; g = lo; b = down; break;
          case 2: r = hi; g = up; b = lo; break;
          case 3: r = down; g = hi; b = lo; break;
          case 4: r = lo; g = hi; b = up; break;
          default: r = lo; g = down; b = hi; break;
        }
        set_dac(i++, (uint8_t)r, (uint8_t)g, (uint8_t)b);
      }
  while (i < 256)
    set_dac(i++, 0, 0, 0);
}

static void debug_ports(int on)
{
  uint32_t cr4;

  __asm__("mov %%cr4, %0" : "=r"(cr4));
  if (on)
  {
    /* I/O breakpoints (CR4.DE) on 3C8h, 3C9h, 3DAh, 3C7h: one byte, read
       or written, in every task */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4 | 0x08));
    __asm__ volatile("mov %0, %%dr0" :: "r"(0x3c8u));
    __asm__ volatile("mov %0, %%dr1" :: "r"(0x3c9u));
    __asm__ volatile("mov %0, %%dr2" :: "r"(0x3dau));
    __asm__ volatile("mov %0, %%dr3" :: "r"(0x3c7u));
    __asm__ volatile("mov %0, %%dr7" :: "r"(0x222200aau | 0x400));
  }
  else
  {
    __asm__ volatile("mov %0, %%dr7" :: "r"(0x400u));
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4 & ~0x08u));
  }
}

static void ports(int on)
{
  uint16_t p;

  for (p = 0x3c0; p <= 0x3cf; p++)
    io_trap(p, on);
  io_trap(0x3d4, on);
  io_trap(0x3d5, on);
  io_trap(0x3da, on);
  debug_ports(on);
}

int vga_active(void)
{
  return active;
}

/* INT 10h AH=00h with mode 13h: 1 if the mode is set */
int vga_enter(int keep)
{
  int i;

  if (!boot.fb_phys || boot.fb_width < W || boot.fb_height < H)
    return 0;
  for (i = 0; i < VGA_PAGES; i++)
    if (!pages[i] && !(pages[i] = page_alloc()))
      return 0;
  for (i = 0; i < VGA_PAGES; i++)
    map_page(VGA_LIN + (uint32_t)i * 4096, pages[i], 7);
  if (!keep)
    memset((void *)VGA_LIN, 0, VGA_PAGES * 4096);
  default_palette();
  widx = wsub = ridx = rsub = 0;
  owner = vm_current();
  if (!active)
    ports(1);
  active = 1;
  last_ms = 0;
  kprintf("vga: mode 13h for session %d (%ux%u on the framebuffer)\n", owner,
          W * (boot.fb_width / W < boot.fb_height / H ? boot.fb_width / W : boot.fb_height / H),
          H * (boot.fb_width / W < boot.fb_height / H ? boot.fb_width / W : boot.fb_height / H));
  return 1;
}

void vga_leave(void)
{
  int i;

  if (!active)
    return;
  active = 0;
  ports(0);
  for (i = 0; i < VGA_PAGES; i++)
    map_page(VGA_LIN + (uint32_t)i * 4096, VGA_LIN + (uint32_t)i * 4096, 7);
}

/* the picture onto the framebuffer (from the timer tick) */
void vga_tick(void)
{
  uint32_t *fb = (uint32_t *)FB_VIRT, pitch = boot.fb_pitch, s, x0, y0, x, y, k, now;
  const uint8_t *src = (const uint8_t *)VGA_LIN;

  if (!active || vm_current() != owner || !vm_has_focus())
    return;
  now = (uint32_t)mdvmm_ms();
  if (last_ms && now - last_ms < 30)
    return;
  last_ms = now ? now : 1;
  s = boot.fb_width / W < boot.fb_height / H ? boot.fb_width / W : boot.fb_height / H;
  x0 = (boot.fb_width - W * s) / 2;
  y0 = (boot.fb_height - H * s) / 2;
  for (y = 0; y < H; y++)
  {
    uint32_t *row = fb + (y0 + y * s) * pitch + x0, *p = row;
    for (x = 0; x < W; x++)
    {
      uint32_t c = pal[src[y * W + x]];
      for (k = 0; k < s; k++)
        *p++ = c;
    }
    for (k = 1; k < s; k++)
      memcpy(row + k * pitch, row, W * s * 4);
  }
  pal_dirty = 0;
}

/* a port of the display card, read or written by a V86 program (through
   the I/O bitmap). 1 if it is one of the monitor's. */
int vga_port_out(uint16_t port, int size, uint32_t v)
{
  if (!active || port < 0x3c0 || port > 0x3da || (port > 0x3cf && port != 0x3d4 && port != 0x3d5 && port != 0x3da))
    return 0;
  if (size != 1)
    return 1;                           /* index and data in one word: of no use here */
  switch (port)
  {
    case 0x3c7:
      ridx = (int)(v & 255);
      rsub = 0;
      dac_reading = 1;
      break;
    case 0x3c8:
      widx = (int)(v & 255);
      wsub = 0;
      dac_reading = 0;
      break;
    case 0x3c9:
      wcomp[wsub++] = (uint8_t)v;
      if (wsub == 3)
      {
        set_dac(widx, wcomp[0], wcomp[1], wcomp[2]);
        widx = (widx + 1) & 255;
        wsub = 0;
      }
      break;
  }
  return 1;
}

static uint8_t status(void)
{
  /* vertical retrace comes and goes, so that loops that wait for either go on */
  return (status_reads++ >> 3) & 1 ? 0x09 : 0x00;
}

int vga_port_in(uint16_t port, int size, uint32_t *v)
{
  if (!active || port < 0x3c0 || port > 0x3da || (port > 0x3cf && port != 0x3d4 && port != 0x3d5 && port != 0x3da))
    return 0;
  *v = 0;
  if (size != 1)
    return 1;
  switch (port)
  {
    case 0x3c7: *v = 3; break;          /* DAC state: ready for writes */
    case 0x3c8: *v = (uint32_t)widx; break;
    case 0x3c6: *v = 0xff; break;
    case 0x3c9:
      *v = dac[ridx][rsub++];
      if (rsub == 3)
      {
        ridx = (ridx + 1) & 255;
        rsub = 0;
      }
      break;
    case 0x3da: *v = status(); break;
  }
  return 1;
}

/* Debug exception in a DPMI client: an I/O breakpoint on one of the four
   ports? The instruction is done: an OUT is taken note of, an IN gets the
   monitor's value. The debug status register says which port. Which way
   it went is read from the instruction before EIP (code[-1]; at a REP
   OUTSB in progress EIP is still at the instruction); where the exception
   comes late (an emulator) that is not the I/O instruction, and the port
   and the palette's state decide. 1 if the exception was the monitor's. */
int vga_debug(struct frame *f, const uint8_t *code, const uint8_t *esi_byte)
{
  static const uint16_t port_of[4] = { 0x3c8, 0x3c9, 0x3da, 0x3c7 };
  uint8_t op = code[-1], op2 = code[-2];
  uint32_t dr6, v;
  int i;

  if (!active)
    return 0;
  __asm__("mov %%dr6, %0" : "=r"(dr6));
  if (dr6 & 0xc000)
    return 0;                           /* single step, task switch: the client's */
  __asm__ volatile("mov %0, %%dr6" :: "r"(dr6 & ~15u));
  for (i = 0; i < 4; i++)
    if (dr6 & (1u << i))
    {
      uint16_t port = port_of[i];
      int outs = code[0] == 0x6e || (code[0] == 0xf3 && code[1] == 0x6e) || op == 0x6e;
      int in;

      if (op == 0xec || op2 == 0xe4)
        in = 1;
      else if (op == 0xee || op2 == 0xe6 || outs)
        in = 0;
      else
        in = port == 0x3da || (port == 0x3c9 && dac_reading);
      if (in)
      {
        if (vga_port_in(port, 1, &v) && (op == 0xec || op2 == 0xe4))
          *(uint8_t *)&f->eax = (uint8_t)v;
      }
      else
        vga_port_out(port, 1, outs ? *esi_byte : (uint8_t)f->eax);
    }
  /* The trap flag set without a single step reported: not the client's
     doing (seen with QEMU, which hands over wrong flags with an IN on a
     breakpoint port); with it the next instruction would trap. */
  if ((f->eflags & EFL_TF) && !(dr6 & 0x4000))
    f->eflags &= ~EFL_TF;
  return 1;                             /* (also one with nothing to say: not the client's) */
}

/* INT 10h in mode 13h. 1 if the function was done here. */
int vga_int10(struct frame *f)
{
  uint8_t ah = *((uint8_t *)&f->eax + 1), al = *(uint8_t *)&f->eax;
  uint16_t bx = (uint16_t)f->ebx, cx = (uint16_t)f->ecx, dx = (uint16_t)f->edx;
  uint8_t *p;
  int i;

  if (!active)
    return 0;
  switch (ah)
  {
    case 0x0c:                          /* write pixel */
      if (cx < W && dx < H)
        *(uint8_t *)(VGA_LIN + dx * W + cx) = al;
      return 1;
    case 0x0d:                          /* read pixel */
      *(uint8_t *)&f->eax = cx < W && dx < H ? *(uint8_t *)(VGA_LIN + dx * W + cx) : 0;
      return 1;
    case 0x0e:                          /* text: not in this mode */
    case 0x09:
    case 0x0a:
    case 0x13:
      return 1;
    case 0x10:
      switch (al)
      {
        case 0x10:                      /* set one colour: BX, DH red, CH green, CL blue */
          set_dac(bx, (uint8_t)(dx >> 8), (uint8_t)(cx >> 8), (uint8_t)cx);
          return 1;
        case 0x12:                      /* set colours BX.., CX of them, from ES:DX */
          p = rm_ptr((uint16_t)f->es, dx);
          for (i = 0; i < cx && bx + i < 256; i++)
            set_dac(bx + i, p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
          return 1;
        case 0x15:                      /* read one colour */
          *((uint8_t *)&f->edx + 1) = dac[bx & 255][0];
          f->ecx = (f->ecx & ~0xffffu) | (uint32_t)(dac[bx & 255][1] << 8 | dac[bx & 255][2]);
          return 1;
        case 0x17:                      /* read colours */
          p = rm_ptr((uint16_t)f->es, dx);
          for (i = 0; i < cx && bx + i < 256; i++)
            memcpy(p + i * 3, dac[bx + i], 3);
          return 1;
      }
      return 1;
  }
  return 0;
}
