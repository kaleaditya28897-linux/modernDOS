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
 *  - "unchained" 256 colours (mode X and its relatives: mode 13h with the
 *    sequencer's chain-4 bit off, which games set for page flipping and
 *    320x240): four memory planes, the card's write modes and latches.
 *    A0000h is unmapped then; every access faults and the instruction is
 *    run by the monitor (vgaemu.c). The picture's height and start come
 *    from the CRT controller's registers.
 * Not here: the 16 colour planar modes (0Dh-12h) and text in mode 13h.
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

/* the registers programs set, and the four planes of the unchained mode */
#define PLANES_VIRT 0xEF000000u         /* 4 x 64 KB (core.h) */
static uint8_t *const plane_mem = (uint8_t *)PLANES_VIRT;
static int planes_mapped;
static int planar;                      /* chain-4 off: A0000h goes through vga_mem_* */
static uint8_t seq[8], gc[16], crtc[32], latch[4];
static uint8_t seq_idx, gc_idx, crtc_idx;
static uint32_t shown_h, shown_s;       /* the picture's geometry on the framebuffer */

#define PLANE(p, a) plane_mem[((uint32_t)(p) << 16) | ((a) & 0xffff)]

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
    /* I/O breakpoints (CR4.DE) over the card's registers, 3C0h-3DFh: four
       of eight ports each, read or written, in every task */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4 | 0x08));
    __asm__ volatile("mov %0, %%dr0" :: "r"(0x3c0u));
    __asm__ volatile("mov %0, %%dr1" :: "r"(0x3c8u));
    __asm__ volatile("mov %0, %%dr2" :: "r"(0x3d0u));
    __asm__ volatile("mov %0, %%dr3" :: "r"(0x3d8u));
    __asm__ volatile("mov %0, %%dr7" :: "r"(0xaaaa00aau | 0x400));
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

static void map_linear(int on)
{
  int i;

  for (i = 0; i < VGA_PAGES; i++)
    if (on)
      map_page(VGA_LIN + (uint32_t)i * 4096, pages[i], 7);
    else
      unmap_page(VGA_LIN + (uint32_t)i * 4096);
}

/* the sequencer's chain-4 bit changed: one byte per pixel at A0000h (on),
   or four planes behind it (off) */
static void set_planar(int on)
{
  uint8_t *lin = (uint8_t *)VGA_LIN;
  uint32_t o;

  if (on == planar)
    return;
  if (on)
  {
    /* as on the card: byte o of the chained memory is plane o & 3 */
    for (o = 0; o < 0x10000; o++)
      PLANE(o & 3, o & ~3u) = lin[o];
    map_linear(0);
  }
  else
  {
    map_linear(1);
    for (o = 0; o < 0x10000; o++)
      lin[o] = PLANE(o & 3, o & ~3u);
  }
  planar = on;
  shown_h = 0;
  kprintf("vga: %s\n", on ? "256 colours unchained (four planes)" : "256 colours chained");
}

/* a byte of video memory read in the planar mode: the latches are loaded */
uint8_t vga_mem_read(uint32_t off)
{
  int p;

  for (p = 0; p < 4; p++)
    latch[p] = PLANE(p, off);
  if (gc[5] & 8)                        /* read mode 1: colour compare */
  {
    uint8_t r = 0xff;
    for (p = 0; p < 4; p++)
      if (gc[7] & (1 << p))
        r &= (uint8_t)~(latch[p] ^ ((gc[2] >> p) & 1 ? 0xff : 0));
    return r;
  }
  return latch[gc[4] & 3];
}

/* ... and written: through the write mode, to the planes of the map mask */
void vga_mem_write(uint32_t off, uint8_t v)
{
  uint8_t mask = seq[2] & 15, bitmask = gc[8], func = (gc[3] >> 3) & 3, rot = gc[3] & 7;
  int mode = gc[5] & 3, p;

  if (mode == 1)
  {
    for (p = 0; p < 4; p++)
      if (mask & (1 << p))
        PLANE(p, off) = latch[p];
    return;
  }
  if (mode == 0 || mode == 3)
    v = (uint8_t)(v >> rot | v << (8 - rot));
  if (mode == 3)
    bitmask &= v;
  for (p = 0; p < 4; p++)
  {
    uint8_t src;
    if (!(mask & (1 << p)))
      continue;
    if (mode == 2)
      src = (v >> p) & 1 ? 0xff : 0;
    else if (mode == 3 || (gc[1] & (1 << p)))
      src = (gc[0] >> p) & 1 ? 0xff : 0;  /* set/reset */
    else
      src = v;
    switch (func)
    {
      case 1: src &= latch[p]; break;
      case 2: src |= latch[p]; break;
      case 3: src ^= latch[p]; break;
    }
    PLANE(p, off) = (uint8_t)((src & bitmask) | (latch[p] & ~bitmask));
  }
}

/* A page fault at A0000h in the planar mode: the instruction is run by
   the monitor. v86: a V86 frame; code32: a 32 bit code segment. 1 if it
   was done. */
int vga_fault(struct frame *f, uint32_t addr, int v86, int code32)
{
  if (!active || !planar || addr < VGA_LIN || addr >= VGA_LIN + 0x10000)
    return 0;
  if (vgaemu(f, v86, code32))
    return 1;
  kprintf("vga: an instruction on video memory the monitor does not know, at %04x:%08x\n", f->cs, f->eip);
  return 0;
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
  if (!planes_mapped)
  {
    for (i = 0; i < 64; i++)
    {
      uint32_t ph = page_alloc();
      if (!ph || !map_page(PLANES_VIRT + (uint32_t)i * 4096, ph, 3))
        return 0;
    }
    planes_mapped = 1;
  }
  planar = 0;
  map_linear(1);
  if (!keep)
  {
    memset((void *)VGA_LIN, 0, VGA_PAGES * 4096);
    memset(plane_mem, 0, 0x40000);
  }
  /* the registers as a VGA BIOS leaves them in mode 13h */
  memset(seq, 0, sizeof(seq));
  memset(gc, 0, sizeof(gc));
  memset(crtc, 0, sizeof(crtc));
  seq[2] = 0x0f;
  seq[4] = 0x0e;                        /* chain-4 */
  gc[5] = 0x40;
  gc[6] = 0x05;
  gc[7] = 0x0f;
  gc[8] = 0xff;
  crtc[0x07] = 0x1f;
  crtc[0x09] = 0x41;                    /* every line twice */
  crtc[0x12] = 0x8f;                    /* 400 lines */
  crtc[0x13] = 40;
  crtc[0x14] = 0x40;
  crtc[0x17] = 0xa3;
  shown_h = 0;
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
  planar = 0;
  ports(0);
  for (i = 0; i < VGA_PAGES; i++)
    map_page(VGA_LIN + (uint32_t)i * 4096, VGA_LIN + (uint32_t)i * 4096, 7);
}

/* the picture onto the framebuffer (from the timer tick) */
void vga_tick(void)
{
  uint32_t *fb = (uint32_t *)FB_VIRT, pitch = boot.fb_pitch, s, x0, y0, x, y, k, now;
  uint32_t h = H, start = 0, rowbytes = 80;
  const uint8_t *src = (const uint8_t *)VGA_LIN;

  if (!active || vm_current() != owner || !vm_has_focus())
    return;
  now = (uint32_t)mdvmm_ms();
  if (last_ms && now - last_ms < 30)
    return;
  last_ms = now ? now : 1;
  if (planar)
  {
    /* the height: the last displayed line, by the lines a row is shown for */
    uint32_t lines = (crtc[0x12] | (uint32_t)(crtc[0x07] & 0x02) << 7 | (uint32_t)(crtc[0x07] & 0x40) << 3) + 1;
    uint32_t per_row = ((crtc[0x09] & 0x1f) + 1) * (crtc[0x09] & 0x80 ? 2 : 1);
    h = lines / per_row;
    if (h < 100 || h > 600)
      h = H;
    start = (uint32_t)crtc[0x0c] << 8 | crtc[0x0d];
    rowbytes = crtc[0x13] ? crtc[0x13] * 2u : 80;
  }
  s = boot.fb_width / W < boot.fb_height / h ? boot.fb_width / W : boot.fb_height / h;
  if (!s)
    return;                             /* it does not fit */
  if (h != shown_h || s != shown_s)
  {
    fbcon_clear();                      /* another size: what was around it goes */
    shown_h = h;
    shown_s = s;
  }
  x0 = (boot.fb_width - W * s) / 2;
  y0 = (boot.fb_height - h * s) / 2;
  for (y = 0; y < h; y++)
  {
    uint32_t *row = fb + (y0 + y * s) * pitch + x0, *p = row;
    uint32_t line = start + y * rowbytes;
    for (x = 0; x < W; x++)
    {
      uint32_t c = pal[planar ? PLANE(x & 3, line + (x >> 2)) : src[y * W + x]];
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
  {
    /* index and data in one word, as programs write the sequencer, the
       graphics and the CRT controller */
    vga_port_out(port, 1, v & 0xff);
    vga_port_out((uint16_t)(port + 1), 1, (v >> 8) & 0xff);
    return 1;
  }
  switch (port)
  {
    case 0x3c4: seq_idx = (uint8_t)(v & 7); break;
    case 0x3c5:
      seq[seq_idx] = (uint8_t)v;
      if (seq_idx == 4)
        set_planar(!(v & 8));
      break;
    case 0x3ce: gc_idx = (uint8_t)(v & 15); break;
    case 0x3cf: gc[gc_idx] = (uint8_t)v; break;
    case 0x3d4: crtc_idx = (uint8_t)(v & 31); break;
    case 0x3d5: crtc[crtc_idx] = (uint8_t)v; break;
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
    case 0x3c4: *v = seq_idx; break;
    case 0x3c5: *v = seq[seq_idx]; break;
    case 0x3ce: *v = gc_idx; break;
    case 0x3cf: *v = gc[gc_idx]; break;
    case 0x3d4: *v = crtc_idx; break;
    case 0x3d5: *v = crtc[crtc_idx]; break;
    case 0x3cc: *v = 0x63; break;       /* miscellaneous output */
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

/* Debug exception in a DPMI client: an I/O breakpoint on the card's
   registers? The instruction is done: an OUT is taken note of, an IN gets
   the monitor's value. Port, size and direction are read from the
   instruction before EIP (code[-1]; at a REP OUTSB in progress EIP is still
   at the instruction; code32: a 32 bit code segment). 1 if the exception
   was the monitor's. */
int vga_debug(struct frame *f, const uint8_t *code, const uint8_t *esi_byte, int code32)
{
  uint8_t op = code[-1], op2 = code[-2];
  uint32_t dr6, v;
  uint16_t port;
  int in, size = 1, outs = 0, known = 1;

  if (!active)
    return 0;
  __asm__("mov %%dr6, %0" : "=r"(dr6));
  if (dr6 & 0xc000)
    return 0;                           /* single step, task switch: the client's */
  __asm__ volatile("mov %0, %%dr6" :: "r"(dr6 & ~15u));
  if (code[0] == 0x6e || (code[0] == 0xf3 && code[1] == 0x6e) || op == 0x6e)
  {
    outs = 1;
    in = 0;
    port = (uint16_t)f->edx;
  }
  else if (op >= 0xec && op <= 0xef)    /* IN/OUT with DX */
  {
    in = op < 0xee;
    if (op & 1)
      size = (code32 != (op2 == 0x66)) ? 4 : 2;
    port = (uint16_t)f->edx;
  }
  else if (op2 >= 0xe4 && op2 <= 0xe7)  /* IN/OUT with a port number */
  {
    in = op2 < 0xe6;
    if (op2 & 1)
      size = (code32 != (code[-3] == 0x66)) ? 4 : 2;
    port = op;
  }
  else
  {
    known = 0;
    in = 0;
    port = 0;
  }
  if (known && in)
  {
    if (vga_port_in(port, 1, &v))
      *(uint8_t *)&f->eax = (uint8_t)v;
    if (size > 1 && vga_port_in((uint16_t)(port + 1), 1, &v))
      *((uint8_t *)&f->eax + 1) = (uint8_t)v;
  }
  else if (known)
    vga_port_out(port, size > 1 ? 2 : 1, outs ? *esi_byte : f->eax & 0xffff);
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
