/* MDVMM core - mouse driver (INT 33h) for UEFI boots
 *
 * A Microsoft compatible mouse driver in the virtual BIOS, fed by USB
 * boot protocol mice (xhci.c). Motion arrives in mickeys and is scaled
 * by the mickey/pixel ratio (function 0Fh, default 8 horizontal and 16
 * vertical mickeys per 8 pixels) into the virtual screen, which is
 * 640x200 in text mode as with every DOS mouse driver: the text cell is
 * x/8, y/8. In text mode the cursor is drawn by fbcon.c as an inverted
 * cell; graphics programs draw their own. Not yet: event handlers
 * (function 0Ch), PS/2 mice.
 */
#include "core.h"

#define BUTTONS 3

static struct {
  int present;
  int x, y;                             /* virtual screen position */
  int fx, fy;                           /* fractions, in 1/8 mickey */
  int buttons;
  int shown;                            /* cursor counter: visible at 0 */
  int xmin, xmax, ymin, ymax;
  int hratio, vratio;                   /* mickeys per 8 pixels */
  int mickey_x, mickey_y;               /* since function 0Bh */
  int presses[BUTTONS], releases[BUTTONS];
  int press_x[BUTTONS], press_y[BUTTONS], release_x[BUTTONS], release_y[BUTTONS];
} m;

static void reset(void)
{
  int present = m.present;

  memset(&m, 0, sizeof(m));
  m.present = present;
  m.xmax = 639;
  m.ymax = 199;
  m.x = 320;
  m.y = 100;
  m.shown = -1;
  m.hratio = 8;
  m.vratio = 16;
}

static void init(void)
{
  static int done;

  if (!done)
  {
    done = 1;
    reset();
  }
}

static int clamp(int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

/* a USB mouse report: buttons (bit 0 left, 1 right, 2 middle), motion */
void mouse_input(int buttons, int dx, int dy)
{
  int i;

  init();
  if (!m.present)
  {
    m.present = 1;
    kprintf("mouse: first report\n");
  }
  m.mickey_x += dx;
  m.mickey_y += dy;
  m.fx += dx * 64 / (m.hratio ? m.hratio : 8);  /* 1/8 pixel units */
  m.fy += dy * 64 / (m.vratio ? m.vratio : 16);
  m.x = clamp(m.x + m.fx / 8, m.xmin, m.xmax);
  m.y = clamp(m.y + m.fy / 8, m.ymin, m.ymax);
  m.fx %= 8;
  m.fy %= 8;
  for (i = 0; i < BUTTONS; i++)
  {
    int bit = 1 << i;
    if ((buttons & bit) && !(m.buttons & bit))
    {
      m.presses[i]++;
      m.press_x[i] = m.x;
      m.press_y[i] = m.y;
    }
    else if (!(buttons & bit) && (m.buttons & bit))
    {
      m.releases[i]++;
      m.release_x[i] = m.x;
      m.release_y[i] = m.y;
    }
  }
  m.buttons = buttons & 7;
}

/* text cell under a visible mouse cursor, or -1 (fbcon.c) */
int mouse_text_cell(void)
{
  if (!m.present || m.shown < 0 || m.y / 8 >= 25 || m.x / 8 >= 80)
    return -1;
  return (m.y / 8) * 80 + m.x / 8;
}

void int33(struct frame *f)
{
  int b;

  init();
  switch (AX(f))
  {
    case 0x0000:                        /* reset and status */
    case 0x0021:                        /* software reset */
      reset();
      AX(f) = 0xffff;                   /* installed (a USB mouse may come later) */
      BX(f) = BUTTONS;
      break;
    case 0x0001:
      if (m.shown < 0)
        m.shown++;
      break;
    case 0x0002:
      m.shown--;
      break;
    case 0x0003:
      BX(f) = (uint16_t)m.buttons;
      CX(f) = (uint16_t)m.x;
      DX(f) = (uint16_t)m.y;
      break;
    case 0x0004:
      m.x = clamp((int16_t)CX(f), m.xmin, m.xmax);
      m.y = clamp((int16_t)DX(f), m.ymin, m.ymax);
      break;
    case 0x0005:                        /* button press information */
      b = BX(f) < BUTTONS ? BX(f) : 0;
      AX(f) = (uint16_t)m.buttons;
      BX(f) = (uint16_t)m.presses[b];
      CX(f) = (uint16_t)m.press_x[b];
      DX(f) = (uint16_t)m.press_y[b];
      m.presses[b] = 0;
      break;
    case 0x0006:                        /* button release information */
      b = BX(f) < BUTTONS ? BX(f) : 0;
      AX(f) = (uint16_t)m.buttons;
      BX(f) = (uint16_t)m.releases[b];
      CX(f) = (uint16_t)m.release_x[b];
      DX(f) = (uint16_t)m.release_y[b];
      m.releases[b] = 0;
      break;
    case 0x0007:
      m.xmin = (int16_t)CX(f);
      m.xmax = (int16_t)DX(f);
      if (m.xmin > m.xmax)
      {
        int t = m.xmin; m.xmin = m.xmax; m.xmax = t;
      }
      m.x = clamp(m.x, m.xmin, m.xmax);
      break;
    case 0x0008:
      m.ymin = (int16_t)CX(f);
      m.ymax = (int16_t)DX(f);
      if (m.ymin > m.ymax)
      {
        int t = m.ymin; m.ymin = m.ymax; m.ymax = t;
      }
      m.y = clamp(m.y, m.ymin, m.ymax);
      break;
    case 0x000b:                        /* motion counters */
      CX(f) = (uint16_t)m.mickey_x;
      DX(f) = (uint16_t)m.mickey_y;
      m.mickey_x = m.mickey_y = 0;
      break;
    case 0x000f:
      m.hratio = CX(f) ? CX(f) : 8;
      m.vratio = DX(f) ? DX(f) : 16;
      break;
    case 0x001b:                        /* get sensitivity */
      BX(f) = CX(f) = 50;
      DX(f) = 0;
      break;
    case 0x0024:                        /* driver version, type */
      BX(f) = 0x0700;
      CX(f) = 0x0400;                   /* PS/2 type, no IRQ */
      break;
  }
}
