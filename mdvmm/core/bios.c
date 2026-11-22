/* MDVMM core - virtual PC BIOS for UEFI boot
 *
 * A UEFI machine has no PC BIOS, but DOS does all its hardware access
 * through one. The monitor provides it: rom.asm is mapped at F000:0000
 * and its interrupt vectors are HLT breakpoints that land here.
 *  - INT 10h: 80x25 text mode on a RAM screen, drawn on the GOP
 *    framebuffer by fbcon.c (and mirrored to COM1 when present); VBE 3.0
 *    with one linear framebuffer mode, the GOP mode itself
 *  - INT 13h: the boot disk image the UEFI loader read from the ESP,
 *    as floppy drive 00h; hard disks 80h+ from native drivers (AHCI, NVMe),
 *    with the EDD extensions (41h-48h)
 *  - INT 16h / IRQ1: PS/2 keyboard, scancode set 1; USB keyboards on
 *    xHCI controllers (xhci.c, usbkbd.c) arrive as PS/2 scancodes
 *  - INT 1Ah / IRQ0: PIT ticks, RTC
 *  - INT 15h: extended memory (E820h/E801h/88h) for an XMS driver: the
 *    loader's region, seen by DOS at 1MB (HMA first); see ext_phys()
 *  - INT 11h, 12h, 14h, 17h, 18h, 19h: the usual small things
 * After setting up the IVT and BIOS data area, the boot sector of the
 * disk image is started at 0000:7C00, as a BIOS would.
 */
#include "core.h"


/* addresses in the ROM table, order of rom.asm */
enum rom_id {
  R_INT10, R_INT11, R_INT12, R_INT13, R_INT14, R_INT15, R_INT16, R_INT17,
  R_INT18, R_INT19, R_INT1A, R_IRQ0_BP, R_IRQ1_BP, R_INT16_WAIT,
  R_DPT, R_CONFIG, R_STUB_BP, R_STUB_ENTRY, R_STUB_PSP, R_STUB_RETF,
  R_IRQ0, R_IRQ1, R_IRQ_MASTER, R_IRQ_SLAVE, R_IRET,
  R_VBE_OEM, R_VBE_VENDOR, R_VBE_PRODUCT, R_VBE_REV, R_VBE_MODES,
  R_COUNT
};

extern const uint8_t rom_image[], rom_image_end[];

static uint16_t rom[R_COUNT];
static int active;
static uint8_t *rd;                     /* disk image */
static uint32_t rd_sectors, rd_spt, rd_heads, rd_cyls;
static uint8_t disk_status;
static int has_serial;

/* BIOS data area */
#define BDA8(o)   (*(volatile uint8_t *)(0x400 + (o)))
#define BDA16(o)  (*(volatile uint16_t *)(0x400 + (o)))
#define BDA32(o)  (*(volatile uint32_t *)(0x400 + (o)))

static void set_flag(struct frame *f, uint32_t flag, int on)
{
  if (on)
    f->eflags |= flag;
  else
    f->eflags &= ~flag;
}

static void set_vec(int v, uint16_t seg, uint16_t off)
{
  ((uint16_t *)0)[v * 2] = off;
  ((uint16_t *)0)[v * 2 + 1] = seg;
}

/* ---------------------------------------------------------------- */
/* extended memory                                                  */

/* DOS extended memory address (as an XMS driver or DPMI client sees it)
   -> physical address. On UEFI boots DOS sees the loader's region at
   1MB; 0 if the address is outside it. Identity on DOS boots. */
uint32_t ext_phys(uint32_t a)
{
  if (boot.boot_type != BOOT_UEFI)
    return a;
  if (a < 0x100000 || a - 0x100000 >= boot.xms_size)
    return 0;
  return boot.xms_phys + (a - 0x100000);
}

static void e820(struct frame *f)
{
  struct entry { uint64_t base, len; uint32_t type; } __attribute__((packed)) e[3];
  uint32_t conv = BDA16(0x13) * 1024u, n = 0, i = f->ebx;

  e[n].base = 0, e[n].len = conv, e[n++].type = 1;
  e[n].base = conv, e[n].len = 0x100000 - conv, e[n++].type = 2;
  if (boot.xms_size)
    e[n].base = 0x100000, e[n].len = boot.xms_size, e[n++].type = 1;
  if (f->edx != 0x534d4150 || i >= n || (f->ecx & 0xffff) < 20)
  {
    AH(f) = 0x86;
    set_flag(f, EFL_CF, 1);
    return;
  }
  memcpy(rm_ptr(f->es, DI(f)), &e[i], 20);
  f->eax = 0x534d4150;
  f->ecx = 20;
  f->ebx = i + 1 < n ? i + 1 : 0;
  set_flag(f, EFL_CF, 0);
}

/* ---------------------------------------------------------------- */
/* serial mirror of the console (COM1, if there is a UART)          */

void serial_putc(uint8_t c)
{
  int i;

  if (!has_serial)
    return;
  for (i = 0; i < 100000 && !(inb(0x3fd) & 0x20); i++)
    ;
  outb(0x3f8, c);
}

static void serial_init(void)
{
  outb(0x3ff, 0x5a);
  if (inb(0x3ff) != 0x5a)
    return;
  outb(0x3f9, 0);                       /* no interrupts */
  outb(0x3fb, 0x80);
  outb(0x3f8, 1);                       /* 115200 baud */
  outb(0x3f9, 0);
  outb(0x3fb, 0x03);                    /* 8N1 */
  outb(0x3fc, 0x03);
  has_serial = 1;
}

/* ---------------------------------------------------------------- */
/* INT 10h: text mode video                                         */

#define COLS  80
#define ROWS  25

static uint16_t *cell(int page, int row, int col)
{
  return (uint16_t *)(0xb8000 + (page & 7) * 0x1000 + (row * COLS + col) * 2);
}

static void get_cursor(int page, int *row, int *col)
{
  uint16_t pos = BDA16(0x50 + (page & 7) * 2);
  *row = pos >> 8;
  *col = pos & 0xff;
}

static void set_cursor(int page, int row, int col)
{
  BDA16(0x50 + (page & 7) * 2) = (uint16_t)(row << 8 | col);
}

static void scroll(int page, int n, uint8_t attr, int top, int left, int bottom, int right, int up)
{
  int r, c, lines;

  if (right >= COLS)
    right = COLS - 1;
  if (bottom >= ROWS)
    bottom = ROWS - 1;
  if (top > bottom || left > right)
    return;
  lines = bottom - top + 1;
  if (n == 0 || n > lines)
    n = lines;
  for (r = 0; r < lines; r++)
  {
    int dst = up ? top + r : bottom - r;
    int src = up ? dst + n : dst - n;
    for (c = left; c <= right; c++)
      *cell(page, dst, c) = r < lines - n ? *cell(page, src, c) : (uint16_t)(attr << 8 | ' ');
  }
}

/* write a character at the cursor and advance; attr < 0 keeps the cell's */
static void put_tty(int page, uint8_t ch, int attr)
{
  int row, col;

  serial_putc(ch);
  get_cursor(page, &row, &col);
  switch (ch)
  {
    case 7:
      return;
    case 8:
      if (col)
        col--;
      break;
    case 10:
      row++;
      break;
    case 13:
      col = 0;
      break;
    default:
    {
      uint16_t *c = cell(page, row, col);
      *c = (uint16_t)((attr < 0 ? *c & 0xff00 : attr << 8) | ch);
      if (++col >= COLS)
      {
        col = 0;
        row++;
      }
    }
  }
  if (row >= ROWS)
  {
    scroll(page, 1, *cell(page, ROWS - 1, 0) >> 8, 0, 0, ROWS - 1, COLS - 1, 1);
    row = ROWS - 1;
  }
  set_cursor(page, row, col);
}

static void set_mode(uint8_t mode)
{
  int i;

  if ((mode & 0x7f) != 3 && (mode & 0x7f) != 2)
    kprintf("bios: video mode %02x not supported, using 3\n", mode);
  BDA8(0x49) = 3;
  BDA16(0x4a) = COLS;
  BDA16(0x4c) = 0x1000;
  BDA16(0x4e) = 0;
  BDA8(0x62) = 0;
  BDA8(0x84) = ROWS - 1;
  BDA16(0x85) = 16;
  BDA16(0x60) = 0x0d0e;
  for (i = 0; i < 8; i++)
    BDA16(0x50 + i * 2) = 0;
  if (!(mode & 0x80))
    for (i = 0; i < 0x4000; i++)
      ((uint16_t *)0xb8000)[i] = 0x0720;
}

/* ---------------------------------------------------------------- */
/* INT 10h AH=4Fh: VBE 3.0 with one mode, the GOP framebuffer       */

#define VBE_MODE  0x141                 /* rom.asm */

static int gfx_mode;                    /* the framebuffer belongs to a program */

static void vbe_info(struct frame *f)
{
  uint8_t *b = rm_ptr(f->es, DI(f));
  int vbe2 = memcmp(b, "VBE2", 4) == 0;

  memset(b, 0, vbe2 ? 512 : 256);
  memcpy(b, "VESA", 4);
  *(uint16_t *)(b + 4) = 0x0300;
  *(uint32_t *)(b + 6) = (uint32_t)ROM_SEG << 16 | rom[R_VBE_OEM];
  *(uint32_t *)(b + 10) = 0;            /* capabilities: fixed DAC, VGA compatible */
  *(uint32_t *)(b + 14) = (uint32_t)ROM_SEG << 16 | rom[R_VBE_MODES];
  *(uint16_t *)(b + 18) = (uint16_t)(boot.fb_size >> 16);
  if (vbe2)
  {
    *(uint16_t *)(b + 20) = 0x0100;
    *(uint32_t *)(b + 22) = (uint32_t)ROM_SEG << 16 | rom[R_VBE_VENDOR];
    *(uint32_t *)(b + 26) = (uint32_t)ROM_SEG << 16 | rom[R_VBE_PRODUCT];
    *(uint32_t *)(b + 30) = (uint32_t)ROM_SEG << 16 | rom[R_VBE_REV];
  }
}

static void vbe_mode_info(struct frame *f)
{
  uint8_t *b = rm_ptr(f->es, DI(f));
  uint32_t pitch = boot.fb_pitch * 4;
  int rgb = boot.fb_format == FB_RGBX;

  memset(b, 0, 256);
  *(uint16_t *)b = 0xfb;                /* supported, color, graphics, LFB only */
  *(uint16_t *)(b + 0x10) = (uint16_t)pitch;
  *(uint16_t *)(b + 0x12) = (uint16_t)boot.fb_width;
  *(uint16_t *)(b + 0x14) = (uint16_t)boot.fb_height;
  b[0x16] = 8;
  b[0x17] = 16;
  b[0x18] = 1;                          /* planes */
  b[0x19] = 32;                         /* bits per pixel */
  b[0x1a] = 1;                          /* banks */
  b[0x1b] = 6;                          /* direct color */
  b[0x1e] = 1;
  b[0x1f] = 8; b[0x20] = rgb ? 0 : 16;  /* red */
  b[0x21] = 8; b[0x22] = 8;             /* green */
  b[0x23] = 8; b[0x24] = rgb ? 16 : 0;  /* blue */
  b[0x25] = 8; b[0x26] = 24;            /* reserved */
  *(uint32_t *)(b + 0x28) = boot.fb_phys;
  *(uint16_t *)(b + 0x32) = (uint16_t)pitch;
  b[0x36] = 8; b[0x37] = rgb ? 0 : 16;
  b[0x38] = 8; b[0x39] = 8;
  b[0x3a] = 8; b[0x3b] = rgb ? 16 : 0;
  b[0x3c] = 8; b[0x3d] = 24;
}

static void graphics(int on)
{
  if (on == gfx_mode)
    return;
  gfx_mode = on;
  fbcon_graphics(on);
}

static void vbe(struct frame *f)
{
  uint16_t mode;
  uint8_t fn = AL(f);

  AX(f) = 0x014f;                       /* failed */
  if (!boot.fb_phys)
    return;
  switch (fn)
  {
    case 0x00:
      vbe_info(f);
      break;
    case 0x01:
      if ((CX(f) & 0x1ff) != VBE_MODE)
        return;
      vbe_mode_info(f);
      break;
    case 0x02:
      mode = BX(f) & 0x1ff;
      if (mode == VBE_MODE)
      {
        graphics(1);
        if (!(BX(f) & 0x8000))
          fbcon_clear();
      }
      else if (mode == 0x03 || mode == 0x02)
      {
        set_mode((uint8_t)(mode | (BX(f) & 0x8000 ? 0x80 : 0)));
        graphics(0);
      }
      else
        return;
      break;
    case 0x03:
      BX(f) = gfx_mode ? VBE_MODE | 0x4000 : BDA8(0x49);
      break;
    case 0x06:                          /* scan line length: get only */
      if (BL(f) != 1)
        return;
      BX(f) = (uint16_t)(boot.fb_pitch * 4);
      CX(f) = (uint16_t)boot.fb_pitch;
      DX(f) = (uint16_t)boot.fb_height;
      break;
    case 0x07:                          /* display start: 0,0 only */
      if (BL(f) == 1)
        CX(f) = DX(f) = 0;
      else if (BL(f) == 0 || BL(f) == 0x80)
      {
        if (CX(f) || DX(f))
          return;
      }
      else
        return;
      break;
    default:
      return;
  }
  AX(f) = 0x004f;
}

static void int10(struct frame *f)
{
  int page = BH(f) & 7, row, col, i;

  switch (AH(f))
  {
    case 0x00:
      set_mode(AL(f));
      graphics(0);                      /* text mode only */
      AL(f) = 0x30;
      break;
    case 0x01:
      BDA16(0x60) = CX(f);
      break;
    case 0x02:
      set_cursor(page, DH(f), DL(f));
      break;
    case 0x03:
      get_cursor(page, &row, &col);
      DX(f) = (uint16_t)(row << 8 | col);
      CX(f) = BDA16(0x60);
      break;
    case 0x05:
      BDA8(0x62) = AL(f) & 7;
      BDA16(0x4e) = (AL(f) & 7) * 0x1000;
      break;
    case 0x06:
    case 0x07:
      scroll(BDA8(0x62), AL(f), BH(f), CH(f), CL(f), DH(f), DL(f), AH(f) == 6);
      break;
    case 0x08:
      get_cursor(page, &row, &col);
      AX(f) = *cell(page, row, col);
      break;
    case 0x09:
    case 0x0a:
      get_cursor(page, &row, &col);
      for (i = 0; i < CX(f) && row * COLS + col + i < ROWS * COLS; i++)
      {
        uint16_t *p = cell(page, row, col + i);   /* wraps to next rows */
        *p = AH(f) == 9 ? (uint16_t)(BL(f) << 8 | AL(f)) : (uint16_t)((*p & 0xff00) | AL(f));
      }
      if (CX(f) == 1)
        serial_putc(AL(f));
      break;
    case 0x0e:
      put_tty(BDA8(0x62), AL(f), -1);
      break;
    case 0x0f:
      AL(f) = BDA8(0x49);
      AH(f) = COLS;
      BH(f) = BDA8(0x62);
      break;
    case 0x11:
      if (AL(f) == 0x30)
      {
        CX(f) = 16;
        DL(f) = ROWS - 1;
        f->es = 0;
        *(uint16_t *)&f->ebp = 0;
      }
      break;
    case 0x12:
      if (BL(f) == 0x10)
      {
        BX(f) = 0x0003;                 /* color, 256K */
        CX(f) = 0x0009;
      }
      break;
    case 0x13:                          /* write string */
    {
      const uint8_t *str = rm_ptr(f->es, 0) + *(uint16_t *)&f->ebp;
      get_cursor(page, &row, &col);
      set_cursor(page, DH(f), DL(f));
      for (i = 0; i < CX(f); i++)
      {
        uint8_t ch = *str++;
        int attr = BL(f);
        if (AL(f) & 2)
          attr = *str++;
        put_tty(page, ch, attr);
      }
      if (!(AL(f) & 1))
        set_cursor(page, row, col);
      break;
    }
    case 0x1a:
      if (AL(f) == 0)
      {
        AL(f) = 0x1a;
        BX(f) = 0x0008;                 /* VGA, color */
      }
      break;
    case 0x4f:
      vbe(f);
      break;
  }
}

/* ---------------------------------------------------------------- */
/* keyboard                                                         */

/* scancode set 1 -> ASCII, 0x00-0x39 */
static const char kb_norm[] =
  "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char kb_shift[] =
  "\0\033!@#$%^&*()_+\b\0QWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

#define KF_RSHIFT 0x01
#define KF_LSHIFT 0x02
#define KF_CTRL   0x04
#define KF_ALT    0x08
#define KF_SCROLL 0x10
#define KF_NUM    0x20
#define KF_CAPS   0x40
#define KF_INS    0x80

static int kb_e0;

static void kb_put(uint16_t key)
{
  uint16_t tail = BDA16(0x1c), next = tail + 2;

  if (next >= BDA16(0x82))
    next = BDA16(0x80);
  if (next == BDA16(0x1a))
    return;                             /* full */
  *(uint16_t *)(0x400 + tail) = key;
  BDA16(0x1c) = next;
}

static void __attribute__((noreturn)) reboot(void)
{
  kprintf("bios: reboot\n");
  outb(0xcf9, 0x02);
  outb(0xcf9, 0x06);
  outb(0x64, 0xfe);
  for (;;)
    __asm__ volatile("cli; hlt");
}

static void kbd_scancode(uint8_t sc)
{
  uint8_t flags = BDA8(0x17), code = sc & 0x7f;
  int up = sc & 0x80, e0 = kb_e0, shift, ctrl, alt;
  uint8_t ascii = 0, scan = code;

  kb_e0 = 0;
  if (sc == 0xe0 || sc == 0xe1)
  {
    kb_e0 = sc == 0xe0;
    return;
  }
  switch (code)                         /* modifiers and locks */
  {
    case 0x2a:
    case 0x36:
      if (e0)
        return;                         /* fake shifts around gray keys */
      if (up)
        flags &= ~(code == 0x2a ? KF_LSHIFT : KF_RSHIFT);
      else
        flags |= code == 0x2a ? KF_LSHIFT : KF_RSHIFT;
      BDA8(0x17) = flags;
      return;
    case 0x1d:
      BDA8(0x17) = up ? flags & ~KF_CTRL : flags | KF_CTRL;
      return;
    case 0x38:
      BDA8(0x17) = up ? flags & ~KF_ALT : flags | KF_ALT;
      return;
    case 0x3a:
      if (!up)
        BDA8(0x17) = flags ^ KF_CAPS;
      return;
    case 0x45:
      if (!up)
        BDA8(0x17) = flags ^ KF_NUM;
      return;
    case 0x46:
      if (!up && !e0)
        BDA8(0x17) = flags ^ KF_SCROLL;
      if (!up && e0 && (flags & KF_CTRL))      /* Ctrl+Break */
      {
        BDA8(0x71) |= 0x80;
        kb_put(0);
      }
      return;
  }
  if (up)
    return;

  shift = flags & (KF_LSHIFT | KF_RSHIFT);
  ctrl = flags & KF_CTRL;
  alt = flags & KF_ALT;

  if (ctrl && alt && code == 0x53)
    reboot();

  if (e0)
  {
    if (code == 0x1c)                   /* keypad Enter */
      ascii = ctrl ? 10 : 13, scan = 0xe0;
    else if (code == 0x35)              /* keypad / */
      ascii = '/', scan = 0xe0;
    else if (code >= 0x47 && code <= 0x53)
    {
      static const uint8_t ctrl_nav[13] = { 0x77, 0x8d, 0x84, 0, 0x73, 0, 0x74, 0, 0x75, 0x91, 0x76, 0x92, 0x93 };
      ascii = 0xe0;
      if (ctrl && ctrl_nav[code - 0x47])
        scan = ctrl_nav[code - 0x47];
      else if (alt)
        scan = code + 0x50, ascii = 0;
    }
    else
      return;
    kb_put((uint16_t)(scan << 8 | ascii));
    return;
  }

  if (code == 0x52 && !(flags & KF_NUM) && !shift)
    BDA8(0x17) ^= KF_INS;

  if (code >= 0x3b && code <= 0x44)     /* F1-F10 */
    scan = alt ? code + 0x2d : ctrl ? code + 0x23 : shift ? code + 0x19 : code;
  else if (code == 0x57 || code == 0x58)        /* F11, F12 */
    scan = (uint8_t)((code == 0x57 ? 0x85 : 0x86) + (alt ? 6 : ctrl ? 4 : shift ? 2 : 0));
  else if (code >= 0x47 && code <= 0x53)        /* keypad */
  {
    static const char digits[] = "789-456+1230.";
    if (code == 0x4a || code == 0x4e)
      ascii = digits[code - 0x47];
    else if (((flags & KF_NUM) != 0) != (shift != 0))
      ascii = digits[code - 0x47];
    else if (ctrl)
    {
      static const uint8_t ctrl_nav[13] = { 0x77, 0x8d, 0x84, 0, 0x73, 0x8f, 0x74, 0, 0x75, 0x91, 0x76, 0x92, 0x93 };
      if (ctrl_nav[code - 0x47])
        scan = ctrl_nav[code - 0x47];
    }
  }
  else if (code < sizeof(kb_norm) - 1)
  {
    char n = kb_norm[code], s = kb_shift[code];
    int letter = n >= 'a' && n <= 'z';
    if (alt)
    {
      if (code >= 0x02 && code <= 0x0d)
        scan = code + 0x76;             /* Alt+1..= : 78h-83h */
      else if (!letter && code != 0x39 && code != 0x0e && code != 0x1c)
        return;
    }
    else if (ctrl)
    {
      if (letter)
        ascii = (uint8_t)(n - 'a' + 1);
      else
        switch (code)
        {
          case 0x01: ascii = 0x1b; break;
          case 0x03: scan = 0x03; break;          /* Ctrl+2: NUL */
          case 0x07: ascii = 0x1e; break;
          case 0x0c: ascii = 0x1f; break;
          case 0x0e: ascii = 0x7f; break;
          case 0x0f: scan = 0x94; break;
          case 0x1a: ascii = 0x1b; break;
          case 0x1b: ascii = 0x1d; break;
          case 0x1c: ascii = 0x0a; break;
          case 0x2b: ascii = 0x1c; break;
          case 0x39: ascii = ' '; break;
          default: return;
        }
    }
    else
    {
      int up_case = (shift != 0) != (letter && (flags & KF_CAPS));
      ascii = (uint8_t)(letter ? (up_case ? s : n) : shift ? s : n);
    }
  }
  else
    return;
  kb_put((uint16_t)(scan << 8 | ascii));
}

static void int16(struct frame *f)
{
  uint16_t head = BDA16(0x1a), key;
  int ext = AH(f) & 0x10;

  xhci_poll();                          /* a key may be waiting */
  switch (AH(f))
  {
    case 0x00:
    case 0x10:
      if (head == BDA16(0x1c))
      {
        f->eip = rom[R_INT16_WAIT];     /* idle until a key arrives */
        return;
      }
      key = *(uint16_t *)(0x400 + head);
      head += 2;
      if (head >= BDA16(0x82))
        head = BDA16(0x80);
      BDA16(0x1a) = head;
      if (!ext && (key & 0xff) == 0xe0 && key >> 8)
        key &= 0xff00;
      AX(f) = key;
      break;
    case 0x01:
    case 0x11:
      if (head == BDA16(0x1c))
      {
        set_flag(f, EFL_ZF, 1);         /* (no yield: DOS polls this on
                                           every call for Ctrl+C) */
        break;
      }
      key = *(uint16_t *)(0x400 + head);
      if (!ext && (key & 0xff) == 0xe0 && key >> 8)
        key &= 0xff00;
      AX(f) = key;
      set_flag(f, EFL_ZF, 0);
      break;
    case 0x02:
      AL(f) = BDA8(0x17);
      break;
    case 0x12:
      AL(f) = BDA8(0x17);
      AH(f) = BDA8(0x18);
      break;
    case 0x05:
    {
      uint16_t tail = BDA16(0x1c);
      kb_put(CX(f));
      AL(f) = BDA16(0x1c) == tail;
      break;
    }
  }
  f->eip++;                             /* continue at RETF 2 */
}

/* ---------------------------------------------------------------- */
/* INT 13h: hard disks 80h+ (native drivers: ahci.c, nvme.c)       */

#define MAX_HD     8
#define HD_HEADS   255
#define HD_SPT     63
#define STAGE_PAGES (DISK_MAX_SECTORS * 512 / 4096)

static struct disk *hds[MAX_HD];
static int nhd;
static uint8_t hd_status;
static uint32_t stage[STAGE_PAGES];     /* pool pages for transfers */

void disk_register(struct disk *d)
{
  int i;

  if (nhd == MAX_HD)
    return;
  if (!stage[0])
    for (i = 0; i < STAGE_PAGES; i++)
      if (!(stage[i] = page_alloc()))
        panic("bios: out of memory");
  hds[nhd] = d;
  kprintf("bios: hard disk %02xh: %s\n", 0x80 + nhd, d->model);
  nhd++;
}

static uint32_t hd_cyls(struct disk *d)
{
  uint32_t c;

  if (d->sectors >= 1024ull * HD_HEADS * HD_SPT)
    return 1024;
  c = (uint32_t)d->sectors / (HD_HEADS * HD_SPT);
  return c ? c : 1;
}

/* move n sectors between the disk and V86 memory at buf; *done = count */
static int hd_transfer(struct disk *d, uint64_t lba, uint32_t n, uint8_t *buf, int write, uint32_t *done)
{
  int err = 0;

  *done = 0;
  if (write && !(boot.flags & BOOTF_DISK_WRITE))
    return 0x03;                        /* write protected */
  while (n && !err)
  {
    uint32_t k = n > DISK_MAX_SECTORS ? DISK_MAX_SECTORS : n, i, bytes = k * 512;

    if (write)
      for (i = 0; i < bytes; i += 4096)
        memcpy(pool_ptr(stage[i / 4096]), buf + i, bytes - i < 4096 ? bytes - i : 4096);
    err = d->rw(d, lba, k, stage, write);
    if (err)
      break;
    if (!write)
      for (i = 0; i < bytes; i += 4096)
        memcpy(buf + i, pool_ptr(stage[i / 4096]), bytes - i < 4096 ? bytes - i : 4096);
    lba += k;
    buf += bytes;
    n -= k;
    *done += k;
  }
  return err;
}

static void int13_hd(struct frame *f)
{
  int idx = DL(f) - 0x80, err = 0;
  struct disk *d = idx < nhd ? hds[idx] : 0;
  uint32_t done;

  if (!d)
  {
    if (AH(f) == 0x15)
    {
      AH(f) = 0;                        /* no such drive */
      set_flag(f, EFL_CF, 0);
      return;
    }
    if (AH(f) == 0x08)
      DL(f) = (uint8_t)nhd;
    AH(f) = 0x01;
    set_flag(f, EFL_CF, 1);
    return;
  }
  switch (AH(f))
  {
    case 0x00:
    case 0x0d:                          /* reset: commit the write cache */
      if (d->flush)
        err = d->flush(d);
      break;
    case 0x04: case 0x0c: case 0x10:
    case 0x11: case 0x14: case 0x44: case 0x47:
      break;                            /* verify, seek, ready */
    case 0x01:
      AH(f) = hd_status;
      set_flag(f, EFL_CF, hd_status != 0);
      return;
    case 0x02:
    case 0x03:
    {
      uint32_t cyl = CH(f) | (uint32_t)(CL(f) & 0xc0) << 2, sec = CL(f) & 0x3f;
      if (!sec || sec > HD_SPT || !AL(f))
      {
        err = 0x04;
        AL(f) = 0;
        break;
      }
      err = hd_transfer(d, ((uint64_t)cyl * HD_HEADS + DH(f)) * HD_SPT + sec - 1, AL(f),
                        rm_ptr(f->es, 0) + BX(f), AH(f) == 0x03, &done);
      AL(f) = (uint8_t)done;
      break;
    }
    case 0x08:
    {
      uint32_t c = hd_cyls(d) - 1;
      AX(f) = 0;
      BL(f) = 0;
      CH(f) = c & 0xff;
      CL(f) = (uint8_t)(HD_SPT | (c >> 2 & 0xc0));
      DH(f) = HD_HEADS - 1;
      DL(f) = (uint8_t)nhd;
      break;
    }
    case 0x15:
      CX(f) = (uint16_t)((d->sectors > 0xffffffffu ? 0xffffffffu : d->sectors) >> 16);
      DX(f) = (uint16_t)(d->sectors > 0xffffffffu ? 0xffffffffu : d->sectors);
      AH(f) = 0x03;                     /* fixed disk */
      set_flag(f, EFL_CF, 0);
      return;
    case 0x41:
      if (BX(f) != 0x55aa)
      {
        err = 0x01;
        break;
      }
      BX(f) = 0xaa55;
      CX(f) = 0x0001;                   /* extended disk access */
      AH(f) = 0x21;                     /* EDD 1.1 */
      set_flag(f, EFL_CF, 0);
      return;
    case 0x42:
    case 0x43:
    {
      uint8_t *dap = rm_ptr(f->ds, SI(f));
      uint16_t *count = (uint16_t *)(dap + 2);
      uint16_t off = *(uint16_t *)(dap + 4), seg = *(uint16_t *)(dap + 6);
      if (dap[0] < 0x10 || (seg == 0xffff && off == 0xffff))
      {
        err = 0x01;                     /* (no 64-bit flat buffers) */
        break;
      }
      err = hd_transfer(d, *(uint64_t *)(dap + 8), *count, rm_ptr(seg, 0) + off,
                        AH(f) == 0x43, &done);
      *count = (uint16_t)done;
      break;
    }
    case 0x48:
    {
      uint8_t *b = rm_ptr(f->ds, SI(f));
      uint16_t size = *(uint16_t *)b;
      if (size < 0x1a)
      {
        err = 0x01;
        break;
      }
      size = size >= 0x1e ? 0x1e : 0x1a;
      memset(b, 0, size);
      *(uint16_t *)b = size;
      *(uint16_t *)(b + 2) = 0x0002;    /* CHS information valid */
      *(uint32_t *)(b + 4) = hd_cyls(d);
      *(uint32_t *)(b + 8) = HD_HEADS;
      *(uint32_t *)(b + 12) = HD_SPT;
      *(uint64_t *)(b + 16) = d->sectors;
      *(uint16_t *)(b + 24) = 512;
      if (size == 0x1e)
        *(uint32_t *)(b + 26) = 0xffffffffu;
      break;
    }
    default:
      err = 0x01;
  }
  hd_status = (uint8_t)err;
  AH(f) = (uint8_t)err;
  set_flag(f, EFL_CF, err != 0);
}

/* ---------------------------------------------------------------- */
/* INT 13h: boot disk image as floppy drive 00h                     */

static void int13(struct frame *f)
{
  uint8_t drive = DL(f);
  int err = 0;

  if (drive >= 0x80)
  {
    int13_hd(f);
    return;
  }
  if (drive != 0 || !rd)
  {
    AH(f) = 0x01;                       /* no such floppy drive */
    set_flag(f, EFL_CF, 1);
    return;
  }
  switch (AH(f))
  {
    case 0x00:
      break;
    case 0x01:
      AH(f) = disk_status;
      set_flag(f, EFL_CF, disk_status != 0);
      return;
    case 0x02:
    case 0x03:
    case 0x04:
    {
      uint32_t cyl = CH(f) | (uint32_t)(CL(f) & 0xc0) << 2;
      uint32_t sec = CL(f) & 0x3f, head = DH(f), n = AL(f), i;
      uint32_t lba = (cyl * rd_heads + head) * rd_spt + sec - 1;
      uint8_t *buf = rm_ptr(f->es, 0) + BX(f);

      if (!sec || sec > rd_spt || head >= rd_heads || !n || lba + n > rd_sectors)
      {
        err = 0x04;                     /* sector not found */
        AL(f) = 0;
        break;
      }
      for (i = 0; i < n && AH(f) != 0x04; i++)
        if (AH(f) == 0x02)
          memcpy(buf + i * 512, rd + (lba + i) * 512, 512);
        else
          memcpy(rd + (lba + i) * 512, buf + i * 512, 512);
      break;
    }
    case 0x05:
      break;
    case 0x08:
    {
      uint32_t c = rd_cyls - 1;
      AX(f) = 0;
      BL(f) = rd_sectors >= 5760 ? 6 : rd_sectors >= 2880 ? 4 : rd_sectors >= 2400 ? 2 : rd_sectors >= 1440 ? 3 : 1;
      CH(f) = c & 0xff;
      CL(f) = (uint8_t)(rd_spt | (c >> 2 & 0xc0));
      DH(f) = (uint8_t)(rd_heads - 1);
      DL(f) = 1;
      f->es = ROM_SEG;
      DI(f) = rom[R_DPT];
      break;
    }
    case 0x15:
      AH(f) = 0x01;                     /* floppy, no change line */
      set_flag(f, EFL_CF, 0);
      return;
    case 0x16:
      break;                            /* not changed */
    case 0x17:
    case 0x18:
      f->es = ROM_SEG;
      DI(f) = rom[R_DPT];
      break;
    default:
      err = 0x01;
  }
  disk_status = (uint8_t)err;
  AH(f) = (uint8_t)err;
  set_flag(f, EFL_CF, err != 0);
}

static void disk_init(void)
{
  uint32_t i, pages = (boot.ramdisk_size + 4095) / 4096;
  const uint8_t *bpb;
  uint32_t total;

  if (!boot.ramdisk_phys || !boot.ramdisk_size || boot.ramdisk_size > RD_MAX)
  {
    kprintf("bios: no boot disk image\n");
    return;
  }
  for (i = 0; i < pages; i++)
    if (!map_page(RD_VIRT + i * 4096, boot.ramdisk_phys + i * 4096, 3))
      panic("bios: cannot map the disk image");
  rd = (uint8_t *)RD_VIRT;
  bpb = rd;
  total = *(const uint16_t *)(bpb + 0x13);
  if (!total)
    total = *(const uint32_t *)(bpb + 0x20);
  rd_spt = *(const uint16_t *)(bpb + 0x18);
  rd_heads = *(const uint16_t *)(bpb + 0x1a);
  if (*(const uint16_t *)(bpb + 0x0b) != 512 || !rd_spt || rd_spt > 63 || !rd_heads || rd_heads > 255)
  {
    rd_spt = 18;                        /* no BPB: assume 1.44 MB */
    rd_heads = 2;
    total = 2880;
  }
  if (total > boot.ramdisk_size / 512)
    total = boot.ramdisk_size / 512;
  rd_sectors = total;
  rd_cyls = (total + rd_spt * rd_heads - 1) / (rd_spt * rd_heads);
  kprintf("bios: disk image %u KB, %u/%u/%u (CHS)\n", boot.ramdisk_size / 1024,
          rd_cyls, rd_heads, rd_spt);
}

/* ---------------------------------------------------------------- */
/* time                                                             */

static uint8_t cmos(uint8_t reg)
{
  outb(0x70, reg);
  return inb(0x71);
}

static void int1a(struct frame *f)
{
  int i;

  set_flag(f, EFL_CF, 0);
  switch (AH(f))
  {
    case 0x00:
      CX(f) = (uint16_t)(BDA32(0x6c) >> 16);
      DX(f) = (uint16_t)BDA32(0x6c);
      AL(f) = BDA8(0x70);
      BDA8(0x70) = 0;
      break;
    case 0x01:
      BDA32(0x6c) = (uint32_t)CX(f) << 16 | DX(f);
      BDA8(0x70) = 0;
      break;
    case 0x02:
    case 0x04:
      for (i = 0; i < 100000 && (cmos(0x0a) & 0x80); i++)
        ;
      if (AH(f) == 2)
      {
        CH(f) = cmos(0x04);
        CL(f) = cmos(0x02);
        DH(f) = cmos(0x00);
        DL(f) = 0;
      }
      else
      {
        CH(f) = cmos(0x32);
        if (CH(f) < 0x19 || CH(f) > 0x21)
          CH(f) = 0x20;
        CL(f) = cmos(0x09);
        DH(f) = cmos(0x08);
        DL(f) = cmos(0x07);
      }
      break;
    case 0x03:
    case 0x05:
      break;                            /* the monitor keeps the RTC */
    default:
      set_flag(f, EFL_CF, 1);
  }
}

/* ---------------------------------------------------------------- */

static void int15(struct frame *f)
{
  set_flag(f, EFL_CF, 0);
  switch (AH(f))
  {
    case 0x4f:                          /* keyboard intercept: keep key */
      set_flag(f, EFL_CF, 1);
      return;
    case 0x24:                          /* A20: always on */
      if (AL(f) == 0x02)
        AL(f) = 1;
      else if (AL(f) == 0x03)
        BX(f) = 0x0003;
      AH(f) = 0;
      return;
    case 0x86:                          /* wait CX:DX microseconds */
    case 0x90:
    case 0x91:
      AH(f) = 0;
      return;
    case 0x88:                          /* KB above 1MB */
      AX(f) = boot.xms_size / 1024 > 0xffff ? 0xffff : (uint16_t)(boot.xms_size / 1024);
      return;
    case 0xe8:
      if (AL(f) == 0x01)                /* KB 1-16MB, 64K blocks above */
      {
        uint32_t kb = boot.xms_size / 1024;
        AX(f) = CX(f) = (uint16_t)(kb > 15 * 1024 ? 15 * 1024 : kb);
        BX(f) = DX(f) = (uint16_t)(kb > 15 * 1024 ? (kb - 15 * 1024) / 64 : 0);
        return;
      }
      if (AL(f) == 0x20)
      {
        e820(f);
        return;
      }
      break;
    case 0xc0:
      f->es = ROM_SEG;
      BX(f) = rom[R_CONFIG];
      AH(f) = 0;
      return;
  }
  AH(f) = 0x86;                         /* not supported */
  set_flag(f, EFL_CF, 1);
}

/* HLT in the ROM: returns 1 if it was a BIOS breakpoint */
int bios_bp(struct frame *f, uint16_t ip)
{
  int id;
  uint16_t *stack;

  if (!active || f->cs != ROM_SEG)
    return 0;
  for (id = 0; id <= R_IRQ1_BP && rom[id] != ip; id++)
    ;
  if (id > R_IRQ1_BP)
    return 0;

  if (id == R_IRQ0_BP)                  /* timer tick */
  {
    if (++BDA32(0x6c) >= 0x1800b0)
    {
      BDA32(0x6c) = 0;
      BDA8(0x70) = 1;
    }
    xhci_poll();                        /* USB keyboards */
    usbkbd_tick();
    fbcon_update();
    f->eip++;
    return 1;
  }
  if (id == R_IRQ1_BP)
  {
    kbd_scancode(AL(f));
    f->eip++;
    return 1;
  }

  switch (id)
  {
    case R_INT10: int10(f); break;
    case R_INT11: AX(f) = BDA16(0x10); break;
    case R_INT12: AX(f) = BDA16(0x13); break;
    case R_INT13: int13(f); break;
    case R_INT14: AH(f) = 0x80; break;          /* timeout: no ports */
    case R_INT15: int15(f); break;
    case R_INT16: int16(f); goto done;          /* sets IP itself */
    case R_INT17: AH(f) = 0x01; break;          /* timeout */
    case R_INT18: panic("bios: no bootable disk (INT 18h)");
    case R_INT19: reboot();
    case R_INT1A: int1a(f); break;
  }
  f->eip++;                             /* continue at RETF 2 */
done:
  /* a BIOS service runs with interrupts as the caller had them */
  stack = rm_ptr(f->ss, (uint16_t)f->esp);
  if (f->eip == (uint32_t)ip + 1)
    vm1.vif = (stack[2] & EFL_IF) != 0;
  if (vm1.vif)
    v86_deliver(f);
  return 1;
}

/* ---------------------------------------------------------------- */

void bios_init(void)
{
  uint32_t size = (uint32_t)(rom_image_end - rom_image), p, i;
  const uint16_t *hdr = (const uint16_t *)(rom_image + 4);
  uint32_t conv = boot.conv_kb;

  if (size > 0xfff0 || *(const uint32_t *)rom_image != 0x5242444d || hdr[0] != R_COUNT)
    panic("bios: bad ROM image");
  for (i = 0; i < R_COUNT; i++)
    rom[i] = hdr[1 + i];

  /* ROM at F0000h and the text screen at B8000h: RAM from the pool */
  for (p = 0; p < 16; p++)
  {
    uint32_t page = page_alloc();
    if (!page || !map_page(0xf0000 + p * 4096, page, 7))
      panic("bios: out of memory");
  }
  memcpy((void *)0xf0000, rom_image, size);
  {
    static const uint8_t tail[16] = {
      0xea, 0, 0, 0x00, 0xf0,           /* FFF0: JMP F000:INT 19h */
      '1', '1', '/', '2', '2', '/', '2', '6',   /* FFF5: BIOS date */
      0, 0xfc, 0                        /* FFFE: model byte AT */
    };
    memcpy((void *)0xffff0, tail, 16);
    *(uint16_t *)0xffff1 = rom[R_INT19];
  }
  for (p = 0; p < VRAM_PAGES; p++)
  {
    vram_phys[p] = page_alloc();
    if (!vram_phys[p] || !map_page(0xb8000 + p * 4096, vram_phys[p], 7))
      panic("bios: out of memory");
  }
  /* HMA: the first 64K of the extended memory region */
  for (p = 0; p < 16; p++)
  {
    uint32_t page = boot.xms_size >= 0x10000 ? boot.xms_phys + p * 4096 : page_alloc();
    if (!page || !map_page(0x100000 + p * 4096, page, 7))
      panic("bios: out of memory");
  }
  boot.stub_seg = ROM_SEG;
  boot.stub_bp = rom[R_STUB_BP];
  boot.stub_entry = rom[R_STUB_ENTRY];
  boot.stub_psp = rom[R_STUB_PSP];
  boot.stub_retf = rom[R_STUB_RETF];

  /* interrupt vectors */
  memset((void *)0, 0, 0x600);
  for (i = 0; i < 0x20; i++)
    set_vec(i, ROM_SEG, rom[R_IRET]);
  for (i = 0; i < 8; i++)
  {
    set_vec(0x08 + i, ROM_SEG, rom[R_IRQ_MASTER]);
    set_vec(0x70 + i, ROM_SEG, rom[R_IRQ_SLAVE]);
  }
  set_vec(0x08, ROM_SEG, rom[R_IRQ0]);
  set_vec(0x09, ROM_SEG, rom[R_IRQ1]);
  for (i = R_INT10; i <= R_INT1A; i++)
    set_vec(0x10 + i, ROM_SEG, rom[i]);
  set_vec(0x1d, 0, 0);
  set_vec(0x1e, ROM_SEG, rom[R_DPT]);
  set_vec(0x1f, 0, 0);
  set_vec(0x40, ROM_SEG, rom[R_INT13]);
  set_vec(0x4a, ROM_SEG, rom[R_IRET]);

  /* BIOS data area */
  if (!conv || conv > 640)
    conv = 640;
  BDA16(0x10) = 0x0021 | 0x0002;        /* 1 floppy, 80x25 color, FPU */
  BDA16(0x13) = (uint16_t)conv;
  BDA16(0x1a) = BDA16(0x1c) = 0x1e;     /* keyboard buffer */
  BDA16(0x80) = 0x1e;
  BDA16(0x82) = 0x3e;
  BDA8(0x17) = KF_NUM;
  BDA8(0x96) = 0x10;                    /* 101 key keyboard */
  BDA16(0x63) = 0x3d4;
  BDA8(0x65) = 0x29;
  BDA8(0x87) = 0x60;
  BDA8(0x88) = 0x09;
  BDA8(0x89) = 0x11;
  BDA16(0x72) = 0x1234;                 /* warm boot */
  set_mode(3);

  /* timer: PIT channel 0, 18.2 Hz; only timer, keyboard, cascade */
  outb(0x43, 0x36);
  outb(0x40, 0);
  outb(0x40, 0);
  outb(0x21, 0xf8);
  outb(0xa1, 0xff);
  while (inb(0x64) & 1)                 /* drain the keyboard controller */
    inb(0x60);

  serial_init();
  disk_init();
  ahci_init();
  nvme_init();
  xhci_init();
  BDA8(0x75) = (uint8_t)nhd;            /* number of hard disks */
  if (nhd)
    kprintf("bios: hard disks are %s\n", boot.flags & BOOTF_DISK_WRITE ? "writable" :
            "read-only (no \\MODERNDOS\\DISKRW on the boot volume)");
  fbcon_init();
  active = 1;

  /* boot: the disk image's boot sector at 0000:7C00, DL = drive */
  if (!rd || rd[510] != 0x55 || rd[511] != 0xaa)
    panic("bios: disk image is not bootable");
  memcpy((void *)0x7c00, rd, 512);
  boot.cs = 0;
  boot.ip = 0x7c00;
  boot.ss = 0;
  boot.sp = 0x7c00;
  boot.ds = boot.es = boot.fs = boot.gs = 0;
  boot.eflags = EFL_IF;
  kprintf("bios: %u KB conventional memory, %u KB extended at %08x, booting the disk image\n",
          conv, boot.xms_size / 1024, boot.xms_phys);
}
