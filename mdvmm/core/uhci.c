/* MDVMM core - USB keyboards and mice on UHCI controllers
 *
 * For machines without xHCI (Intel chipsets up to ICH10, older Macs): the
 * USB 1.1 host controller, polled. It serves HID boot keyboards and mice
 * on its root ports and behind hubs.
 *
 * USB 2.0 ports belong to an EHCI controller with UHCI "companions". There
 * is no EHCI driver here; instead the EHCI controllers are taken from the
 * firmware and left unconfigured, which routes every port to its
 * companion. High speed devices (a USB 2.0 hub, as in Apple keyboards)
 * then work at full speed, and hubs need no transaction translators.
 *
 * Schedule: every frame runs the interrupt queue heads (one per HID
 * endpoint, a ring of transfer descriptors each) and then the control
 * queue head. Control transfers are done synchronously at start-up; the
 * reports are collected on timer ticks (uhci_poll). Devices are found at
 * start-up only.
 */
#include "core.h"

#define MAX_HC      8
#define MAX_EP      8                   /* HID endpoints, all controllers */
#define NTD         8                   /* reports queued per endpoint */
#define MAX_DEPTH   4                   /* hubs behind hubs */

/* I/O registers */
#define USBCMD      0x00
#define USBSTS      0x02
#define USBINTR     0x04
#define FRNUM       0x06
#define FRBASEADD   0x08
#define SOFMOD      0x0c
#define PORTSC(n)   (0x10 + (n) * 2)

#define CMD_RS      0x0001
#define CMD_HCRESET 0x0002
#define CMD_GRESET  0x0004
#define CMD_CF      0x0040
#define CMD_MAXP    0x0080

#define PORT_CCS    0x0001
#define PORT_CSC    0x0002
#define PORT_PE     0x0004
#define PORT_PEC    0x0008
#define PORT_LSDA   0x0100
#define PORT_PR     0x0200

/* transfer descriptor */
#define TD_ACTIVE   (1u << 23)
#define TD_STALLED  (1u << 22)
#define TD_ERRORS   (0x7eu << 16)       /* stalled, buffer, babble, NAK, CRC, bitstuff */
#define TD_FATAL    (0x76u << 16)       /* all but NAK */
#define TD_LS       (1u << 26)
#define TD_CERR3    (3u << 27)
#define TD_SPD      (1u << 29)
#define PID_SETUP   0x2d
#define PID_IN      0x69
#define PID_OUT     0xe1
#define LINK_T      1u
#define LINK_QH     2u
#define LINK_VF     4u

struct td {
  volatile uint32_t link, status, token, buffer;
  uint32_t pad[4];                      /* 32 bytes: 16 byte alignment and room */
};

struct qh {
  volatile uint32_t link, element;
  uint32_t pad[2];
};

struct uhci {
  uint16_t io;
  uint32_t frames;                      /* frame list page */
  uint32_t page;                        /* queue heads, control TDs, data */
  int next_addr;
};

/* layout of uhci.page */
#define QH_START    0x000               /* every frame starts here */
#define QH_CTRL     0x010
#define CTRL_TDS    0x040               /* up to 62 */
#define CTRL_MAXTD  62
#define CTRL_SETUP  0x800
#define CTRL_DATA   0x840               /* up to 1984 bytes */
#define CTRL_MAXDATA 1984

struct hidep {
  struct uhci *hc;
  int mouse;
  uint32_t page;                        /* QH at 0, TDs at 0x20.., buffers at 0x800.. */
  uint32_t token;                       /* without the toggle */
  uint32_t status;
  uint32_t next;                        /* next TD to complete */
};

static struct uhci hcs[MAX_HC];
static struct hidep eps[MAX_EP];
static int nhc, nep;

static void mdelay(uint32_t ms)
{
  udelay(ms * 1000);
}

/* ---------------------------------------------------------------- */
/* control transfers                                                */

/* one control transfer on device `addr` (0: the default address);
   returns the number of data bytes moved, -1 on failure */
static int control(struct uhci *h, int addr, int ls, int mps, uint8_t type, uint8_t req,
                   uint16_t value, uint16_t index, uint16_t len, void *data)
{
  uint8_t *page = pool_ptr(h->page);
  struct qh *qc = (struct qh *)(page + QH_CTRL);
  struct td *td = (struct td *)(page + CTRL_TDS);
  uint8_t *setup = page + CTRL_SETUP, *buf = page + CTRL_DATA;
  uint32_t base = h->page, flags = TD_ACTIVE | TD_CERR3 | (ls ? TD_LS : 0), dev = (uint32_t)addr << 8;
  int in = type & 0x80, n = 0, i, toggle = 1, moved = 0, ms;
  uint16_t left = len;

  if (len > CTRL_MAXDATA || (len + mps - 1) / mps + 2 > CTRL_MAXTD)
    return -1;
  setup[0] = type;
  setup[1] = req;
  setup[2] = (uint8_t)value;
  setup[3] = (uint8_t)(value >> 8);
  setup[4] = (uint8_t)index;
  setup[5] = (uint8_t)(index >> 8);
  setup[6] = (uint8_t)len;
  setup[7] = (uint8_t)(len >> 8);
  if (!in && len)
    memcpy(buf, data, len);

  td[n].status = flags;
  td[n].token = PID_SETUP | dev | 7u << 21;
  td[n].buffer = base + CTRL_SETUP;
  n++;
  while (left)
  {
    uint16_t k = left > mps ? (uint16_t)mps : left;
    td[n].status = flags | (in ? TD_SPD : 0);
    td[n].token = (in ? PID_IN : PID_OUT) | dev | (uint32_t)toggle << 19 | (uint32_t)(k - 1) << 21;
    td[n].buffer = base + CTRL_DATA + (len - left);
    toggle ^= 1;
    left -= k;
    n++;
  }
  td[n].status = flags;                 /* status stage: other direction, DATA1 */
  td[n].token = (in ? PID_OUT : PID_IN) | dev | 1u << 19 | 0x7ffu << 21;
  td[n].buffer = 0;
  n++;
  for (i = 0; i < n; i++)
    td[i].link = i + 1 < n ? (base + CTRL_TDS + (i + 1) * sizeof(struct td)) | LINK_VF : LINK_T;
  qc->element = base + CTRL_TDS;

  for (ms = 0; ms < 2000; ms++)
  {
    int a;                              /* the first TD not done yet */

    for (a = 0; a < n && !(td[a].status & TD_ACTIVE); a++)
      if (td[a].status & TD_FATAL)
      {
        qc->element = LINK_T;
        return -1;
      }
    if (a == n)
      break;
    /* a short data packet ends the data stage and stops the queue (SPD):
       drop the remaining data TDs and go on with the status stage */
    if (in && a >= 2 && a < n - 1 &&
        ((td[a - 1].status + 1) & 0x7ff) < (((td[a - 1].token >> 21) + 1) & 0x7ff))
    {
      for (i = a; i < n - 1; i++)
        td[i].status = 0x7ff;           /* inactive, no data */
      qc->element = base + CTRL_TDS + (n - 1) * sizeof(struct td);
    }
    mdelay(1);
  }
  qc->element = LINK_T;
  if (ms == 2000)
  {
    mdelay(2);                          /* the controller may still be in it */
    return -1;
  }
  for (i = 1; i < n - 1; i++)
    moved += (int)((td[i].status + 1) & 0x7ff);
  if (in && moved)
    memcpy(data, buf, (size_t)moved);
  return moved;
}

/* ---------------------------------------------------------------- */
/* devices                                                          */

static void hid_endpoint(struct uhci *h, int addr, int ls, int ep, int mps, int mouse)
{
  struct hidep *e;
  uint8_t *page;
  struct qh *q, *start;
  struct td *td;
  int i;

  if (nep == MAX_EP)
    return;
  e = &eps[nep];
  memset(e, 0, sizeof(*e));
  if (!(e->page = page_alloc()))
    return;
  if (mps > 64)
    mps = 64;
  if (mps < 3)
    mps = 3;
  page = pool_ptr(e->page);
  q = (struct qh *)page;
  td = (struct td *)(page + 0x20);
  e->hc = h;
  e->mouse = mouse;
  e->token = PID_IN | (uint32_t)addr << 8 | (uint32_t)(ep & 0xf) << 15 | (uint32_t)(mps - 1) << 21;
  e->status = TD_ACTIVE | TD_CERR3 | (ls ? TD_LS : 0);
  for (i = 0; i < NTD; i++)             /* a ring: the controller takes one per frame */
  {
    td[i].link = e->page + 0x20 + ((i + 1) % NTD) * sizeof(struct td);
    td[i].token = e->token | (uint32_t)(i & 1) << 19;   /* DATA0, DATA1, ... */
    td[i].buffer = e->page + 0x800 + i * 64;
    td[i].status = e->status;
  }
  q->element = e->page + 0x20;
  start = (struct qh *)((uint8_t *)pool_ptr(h->page) + QH_START);
  q->link = start->link;
  start->link = e->page | LINK_QH;
  nep++;
}

static void hub(struct uhci *h, int addr, int mps, int depth);

/* a device answers at address 0: give it an address and look at it */
static void new_device(struct uhci *h, int ls, int depth, const char *where)
{
  uint8_t d[18], cfg[256];
  int mps = 8, addr, total, off, n, cur = -1, nif = 0, i, is_hub = 0;
  struct { int ifnum, ep, mps, mouse; } ifs[2];

  if (h->next_addr > 127)
    return;
  if (control(h, 0, ls, 8, 0x80, 6, 0x0100, 0, 8, d) < 8)
  {
    kprintf("uhci: %s: no answer from the device\n", where);
    return;
  }
  if (d[7] == 16 || d[7] == 32 || d[7] == 64)
    mps = d[7];
  addr = h->next_addr++;
  if (control(h, 0, ls, mps, 0x00, 5, (uint16_t)addr, 0, 0, 0) < 0)
  {
    kprintf("uhci: %s: cannot set the address\n", where);
    return;
  }
  mdelay(10);
  if (control(h, addr, ls, mps, 0x80, 6, 0x0100, 0, 18, d) < 18 ||
      control(h, addr, ls, mps, 0x80, 6, 0x0200, 0, 9, cfg) < 9)
  {
    kprintf("uhci: %s: cannot read the descriptors\n", where);
    return;
  }
  total = cfg[2] | cfg[3] << 8;
  if (total > (int)sizeof(cfg))
    total = sizeof(cfg);
  n = control(h, addr, ls, mps, 0x80, 6, 0x0200, 0, (uint16_t)total, cfg);
  if (n < 9)
    return;
  total = n;
  is_hub = d[4] == 9;
  for (off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off])
  {
    if (cfg[off + 1] == 4 && off + 9 <= total)          /* interface */
    {
      cur = -1;
      if (cfg[off + 5] == 9)
        is_hub = 1;
      if (nif < 2 && cfg[off + 5] == 3 && cfg[off + 6] == 1 &&
          (cfg[off + 7] == 1 || cfg[off + 7] == 2))     /* HID boot keyboard / mouse */
      {
        cur = nif++;
        ifs[cur].ifnum = cfg[off + 2];
        ifs[cur].mouse = cfg[off + 7] == 2;
        ifs[cur].ep = 0;
      }
    }
    else if (cfg[off + 1] == 5 && off + 7 <= total && cur >= 0 && !ifs[cur].ep &&
             (cfg[off + 2] & 0x80) && (cfg[off + 3] & 3) == 3)   /* interrupt IN */
    {
      ifs[cur].ep = cfg[off + 2];
      ifs[cur].mps = (cfg[off + 4] | cfg[off + 5] << 8) & 0x7ff;
    }
  }
  kprintf("uhci: %s: device %04x:%04x, address %d%s\n", where, d[8] | d[9] << 8,
          d[10] | d[11] << 8, addr, ls ? ", low speed" : "");
  if (control(h, addr, ls, mps, 0x00, 9, cfg[5], 0, 0, 0) < 0)    /* SET_CONFIGURATION */
    return;
  if (is_hub)
  {
    if (depth < MAX_DEPTH)
      hub(h, addr, mps, depth + 1);
    return;
  }
  for (i = 0; i < nif; i++)
  {
    if (!ifs[i].ep)
      continue;
    control(h, addr, ls, mps, 0x21, 0x0b, 0, (uint16_t)ifs[i].ifnum, 0, 0);   /* boot protocol */
    control(h, addr, ls, mps, 0x21, 0x0a, 0, (uint16_t)ifs[i].ifnum, 0, 0);   /* SET_IDLE: on change */
    hid_endpoint(h, addr, ls, ifs[i].ep, ifs[i].mps, ifs[i].mouse);
    kprintf("uhci: %s: %s\n", where, ifs[i].mouse ? "mouse" : "keyboard");
  }
}

/* a hub: power its ports, reset the connected ones, look at the devices */
static void hub(struct uhci *h, int addr, int mps, int depth)
{
  uint8_t d[16], st[4];
  int ports, p, i;
  char where[24];

  if (control(h, addr, 0, mps, 0xa0, 6, 0x2900, 0, 8, d) < 7)
    return;
  ports = d[2];
  if (ports > 15)
    ports = 15;
  kprintf("uhci: hub at address %d, %d ports\n", addr, ports);
  for (p = 1; p <= ports; p++)
    control(h, addr, 0, mps, 0x23, 3, 8, (uint16_t)p, 0, 0);    /* PORT_POWER */
  mdelay(d[5] * 2 + 100);
  for (p = 1; p <= ports; p++)
  {
    if (control(h, addr, 0, mps, 0xa3, 0, 0, (uint16_t)p, 4, st) < 4 || !(st[0] & 1))
      continue;                         /* nothing connected */
    control(h, addr, 0, mps, 0x23, 3, 4, (uint16_t)p, 0, 0);    /* PORT_RESET */
    for (i = 0; i < 50; i++)
    {
      mdelay(10);
      if (control(h, addr, 0, mps, 0xa3, 0, 0, (uint16_t)p, 4, st) == 4 && !(st[0] & 0x10) &&
          (st[2] & 0x10))
        break;
    }
    control(h, addr, 0, mps, 0x23, 1, 20, (uint16_t)p, 0, 0);   /* clear C_PORT_RESET */
    control(h, addr, 0, mps, 0x23, 1, 16, (uint16_t)p, 0, 0);   /* clear C_PORT_CONNECTION */
    if (!(st[0] & 2))
      continue;                         /* not enabled */
    mdelay(20);
    where[0] = 'h'; where[1] = 'u'; where[2] = 'b'; where[3] = ' ';
    where[4] = (char)('0' + addr / 10 % 10); where[5] = (char)('0' + addr % 10);
    where[6] = ' '; where[7] = 'p'; where[8] = 'o'; where[9] = 'r'; where[10] = 't'; where[11] = ' ';
    where[12] = (char)('0' + p / 10); where[13] = (char)('0' + p % 10); where[14] = 0;
    new_device(h, st[1] & 2 ? 1 : 0, depth, where);     /* bit 9: low speed */
  }
}

/* ---------------------------------------------------------------- */
/* controllers                                                      */

static void root_port(struct uhci *h, int n, int index)
{
  uint16_t reg = h->io + PORTSC(n), v;
  char where[] = "controller 0 port 0";
  int i;

  v = inw(reg);
  if (!(v & PORT_CCS))
    return;
  outw(reg, PORT_PR);
  mdelay(50);
  outw(reg, 0);
  mdelay(10);
  for (i = 0; i < 10; i++)              /* enable; changes are cleared by writing 1 */
  {
    outw(reg, PORT_PE | PORT_CSC | PORT_PEC);
    mdelay(10);
    v = inw(reg);
    if (v & PORT_PE)
      break;
  }
  where[11] = (char)('0' + index);
  where[18] = (char)('1' + n);
  if (!(v & PORT_PE))
  {
    kprintf("uhci: %s: device does not come up (status %04x)\n", where, v);
    return;
  }
  mdelay(20);
  new_device(h, v & PORT_LSDA ? 1 : 0, 0, where);
}

static void uhci_found(int bus, int dev, int fn)
{
  struct uhci *h;
  uint32_t *frames;
  struct qh *q;
  uint16_t io = (uint16_t)(pci_read32(bus, dev, fn, 0x20) & ~3u);
  int i;

  if (nhc == MAX_HC || !io)
    return;
  h = &hcs[nhc];
  memset(h, 0, sizeof(*h));
  h->io = io;
  h->next_addr = 1;
  if (!(h->frames = page_alloc()) || !(h->page = page_alloc()))
    return;
  pci_enable(bus, dev, fn);
  /* no legacy keyboard emulation, no SMIs: the controller is ours */
  pci_write32(bus, dev, fn, 0xc0, (pci_read32(bus, dev, fn, 0xc0) & 0xffff0000u) | 0x8f00);
  outw(io + USBCMD, CMD_GRESET);
  mdelay(50);
  outw(io + USBCMD, 0);
  mdelay(10);
  outw(io + USBCMD, CMD_HCRESET);
  for (i = 0; i < 100 && (inw(io + USBCMD) & CMD_HCRESET); i++)
    mdelay(1);
  outw(io + USBINTR, 0);                /* polled */
  outw(io + USBSTS, 0x3f);

  q = (struct qh *)((uint8_t *)pool_ptr(h->page) + QH_START);
  q->element = LINK_T;
  q->link = (h->page + QH_CTRL) | LINK_QH;
  q = (struct qh *)((uint8_t *)pool_ptr(h->page) + QH_CTRL);
  q->element = LINK_T;
  q->link = LINK_T;
  frames = pool_ptr(h->frames);
  for (i = 0; i < 1024; i++)
    frames[i] = (h->page + QH_START) | LINK_QH;
  outl(io + FRBASEADD, h->frames);
  outw(io + FRNUM, 0);
  outb(io + SOFMOD, 0x40);
  outw(io + USBCMD, CMD_RS | CMD_CF | CMD_MAXP);
  mdelay(100);                          /* ports settle (and come over from EHCI) */
  kprintf("uhci: controller %d at %02x:%02x.%x, ports %04x %04x\n", nhc, bus, dev, fn,
          inw(io + PORTSC(0)), inw(io + PORTSC(1)));
  nhc++;
  for (i = 0; i < 2; i++)
    root_port(h, i, nhc - 1);
}

/* EHCI: take it from the firmware, stop it, leave it unconfigured: all
   ports then belong to the UHCI companions */
static void ehci_found(int bus, int dev, int fn)
{
  volatile uint8_t *cap, *op;
  uint32_t hcc, eecp;
  int i;

  pci_enable(bus, dev, fn);
  cap = mmio_map(pci_bar(bus, dev, fn, 0x10), 0x400);
  if (!cap)
    return;
  hcc = *(volatile uint32_t *)(cap + 8);
  for (eecp = (hcc >> 8) & 0xff; eecp >= 0x40; eecp = (pci_read32(bus, dev, fn, (int)eecp) >> 8) & 0xff)
  {
    uint32_t v = pci_read32(bus, dev, fn, (int)eecp);
    if ((v & 0xff) != 1)
      continue;                         /* not the legacy support capability */
    pci_write32(bus, dev, fn, (int)eecp, v | 1u << 24);          /* OS owns it */
    for (i = 0; i < 100 && (pci_read32(bus, dev, fn, (int)eecp) & (1u << 16)); i++)
      mdelay(10);
    pci_write32(bus, dev, fn, (int)eecp + 4, 0);                  /* no SMIs */
    break;
  }
  op = cap + cap[0];
  *(volatile uint32_t *)(op + 0x00) &= ~1u;                       /* stop */
  for (i = 0; i < 50 && !(*(volatile uint32_t *)(op + 0x04) & 0x1000); i++)
    mdelay(1);
  *(volatile uint32_t *)(op + 0x00) = 2;                          /* reset */
  for (i = 0; i < 100 && (*(volatile uint32_t *)(op + 0x00) & 2); i++)
    mdelay(1);
  *(volatile uint32_t *)(op + 0x08) = 0;                          /* no interrupts */
  *(volatile uint32_t *)(op + 0x40) = 0;                          /* CONFIGFLAG: companions */
  kprintf("ehci: controller at %02x:%02x.%x left to its UHCI companions\n", bus, dev, fn);
}

void uhci_init(void)
{
  pci_scan(0x0c0320, ehci_found);
  mdelay(100);
  pci_scan(0x0c0300, uhci_found);
}

/* timer tick: collect the reports */
void uhci_poll(void)
{
  int i;

  for (i = 0; i < nep; i++)
  {
    struct hidep *e = &eps[i];
    uint8_t *page = pool_ptr(e->page);
    struct td *td = (struct td *)(page + 0x20);
    int n;

    for (n = 0; n < NTD; n++)
    {
      struct td *t = &td[e->next % NTD];
      uint32_t st = t->status;
      const uint8_t *r = page + 0x800 + (e->next % NTD) * 64;
      if (st & TD_ACTIVE)
        break;
      if (!(st & TD_FATAL))
      {
        int len = (int)((st + 1) & 0x7ff);
        if (e->mouse && len >= 3)
          mouse_input(r[0] & 7, (int8_t)r[1], (int8_t)r[2]);
        else if (!e->mouse && len >= 8)
          usbkbd_report(r);
      }
      /* again: the ring has an even number of TDs, so each one keeps its
         data toggle */
      t->status = e->status;
      e->next++;
    }
  }
}
