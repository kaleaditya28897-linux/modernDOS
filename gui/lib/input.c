/* mdgui - input events: keyboard through INT 16h, mouse through INT 33h
 *
 * The mouse driver (CTMOUSE on DOS boots, the MDVMM virtual BIOS on UEFI
 * boots) is set to the screen size with 1:1 motion. Button presses and
 * releases come from the driver's counters (functions 05h/06h), so a
 * quick click between two polls is not lost.
 */
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include "mdgui.h"

#define QSIZE 32

static int mouse, mx, my, mbuttons;
static struct event queue[QSIZE];
static int qhead, qtail;

static void push(const struct event *e)
{
  int next = (qtail + 1) % QSIZE;
  if (next != qhead)
  {
    queue[qtail] = *e;
    qtail = next;
  }
}

static void int33(__dpmi_regs *r)
{
  __dpmi_int(0x33, r);
}

int input_open(void)
{
  const struct gfx_mode *m = gfx_mode();
  __dpmi_regs r;
  unsigned long vec;

  qhead = qtail = 0;
  mouse = 0;
  /* a driver is installed if INT 33h points somewhere */
  dosmemget(0x33 * 4, 4, &vec);
  if (!vec)
    return 0;
  memset(&r, 0, sizeof(r));
  int33(&r);                            /* reset */
  if (r.x.ax != 0xffff)
    return 0;
  mouse = 1;
  r.x.ax = 0x0007;                      /* horizontal range */
  r.x.cx = 0;
  r.x.dx = (unsigned short)(m->w - 1);
  int33(&r);
  r.x.ax = 0x0008;                      /* vertical range */
  r.x.cx = 0;
  r.x.dx = (unsigned short)(m->h - 1);
  int33(&r);
  r.x.ax = 0x000f;                      /* 8 mickeys per 8 pixels both ways */
  r.x.cx = 8;
  r.x.dx = 8;
  int33(&r);
  r.x.ax = 0x0013;                      /* no double speed */
  r.x.dx = 0x7fff;
  int33(&r);
  mx = m->w / 2;
  my = m->h / 2;
  r.x.ax = 0x0004;
  r.x.cx = (unsigned short)mx;
  r.x.dx = (unsigned short)my;
  int33(&r);
  mbuttons = 0;
  return 1;
}

void input_close(void)
{
  __dpmi_regs r;

  if (mouse)
  {
    memset(&r, 0, sizeof(r));
    int33(&r);                          /* reset: text mode defaults */
  }
  mouse = 0;
}

static void poll_mouse(void)
{
  __dpmi_regs r;
  struct event e;
  int b, x, y, buttons;

  memset(&r, 0, sizeof(r));
  r.x.ax = 0x0003;
  int33(&r);
  x = (short)r.x.cx;
  y = (short)r.x.dx;
  buttons = r.x.bx & 7;
  memset(&e, 0, sizeof(e));
  if (x != mx || y != my)
  {
    e.type = EV_MOUSE_MOVE;
    e.x = mx = x;
    e.y = my = y;
    e.buttons = mbuttons;
    push(&e);
  }
  for (b = 0; b < 3; b++)
  {
    static const int bits[3] = { MB_LEFT, MB_RIGHT, MB_MIDDLE };
    int presses, releases;

    r.x.ax = 0x0005;
    r.x.bx = (unsigned short)b;
    int33(&r);
    presses = r.x.bx;
    if (presses)
    {
      e.type = EV_MOUSE_DOWN;
      e.x = (short)r.x.cx;
      e.y = (short)r.x.dx;
      e.button = bits[b];
      mbuttons |= bits[b];
      e.buttons = mbuttons;
      push(&e);
    }
    r.x.ax = 0x0006;
    r.x.bx = (unsigned short)b;
    int33(&r);
    releases = r.x.bx;
    if (releases || (presses && !(buttons & bits[b])))
    {
      e.type = EV_MOUSE_UP;
      e.x = (short)r.x.cx;
      e.y = (short)r.x.dx;
      e.button = bits[b];
      mbuttons &= ~bits[b];
      e.buttons = mbuttons;
      push(&e);
    }
  }
}

static void poll_keyboard(void)
{
  __dpmi_regs r;
  struct event e;

  memset(&r, 0, sizeof(r));
  r.h.ah = 0x11;                        /* key waiting? (extended) */
  __dpmi_int(0x16, &r);
  if (r.x.flags & 0x40)                 /* ZF: none */
    return;
  r.h.ah = 0x10;
  __dpmi_int(0x16, &r);
  memset(&e, 0, sizeof(e));
  e.type = EV_KEY;
  e.scan = r.h.ah;
  e.ascii = r.h.al == 0xe0 ? 0 : r.h.al;
  e.x = mx;
  e.y = my;
  e.buttons = mbuttons;
  r.h.ah = 0x12;
  __dpmi_int(0x16, &r);
  e.shift = r.x.ax;
  push(&e);
}

int input_poll(struct event *e)
{
  if (qhead == qtail)
  {
    poll_keyboard();
    if (mouse)
      poll_mouse();
  }
  if (qhead == qtail)
  {
    e->type = EV_NONE;
    return 0;
  }
  *e = queue[qhead];
  qhead = (qhead + 1) % QSIZE;
  return 1;
}

void input_wait(struct event *e)
{
  __dpmi_regs r;

  while (!input_poll(e))
  {
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x1680;                    /* idle: release the time slice */
    __dpmi_int(0x2f, &r);
  }
}
