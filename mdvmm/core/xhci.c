/* MDVMM core - xHCI (USB 3) host controller driver for USB keyboards
 *
 * Takes each xHCI controller from the firmware (legacy support handoff),
 * resets it and sets up the command ring, one event ring and the device
 * context array. Every device on a root port is addressed; up to two HID
 * boot interfaces of each (keyboard, mouse) are configured and get a ring
 * of interrupt IN transfers. Reports go to usbkbd.c and mouse.c.
 *
 * No interrupts: the event ring is polled on every timer tick and from
 * INT 16h. Not yet: devices behind hubs, hotplug after boot, keyboard
 * LEDs. All rings and contexts are pool pages (below 4GB).
 */
#include "core.h"

#define MAX_HC       4
#define MAX_KBD      8                  /* HID interfaces, all devices */
#define MAX_SLOTS    16
#define NBUF         8                  /* queued report transfers */
#define MAX_IF       2                  /* HID interfaces used per device */

/* capability registers */
#define CAP_LENGTH   0x00
#define CAP_HCS1     0x04
#define CAP_HCS2     0x08
#define CAP_HCC1     0x10
#define CAP_DBOFF    0x14
#define CAP_RTSOFF   0x18
/* operational registers */
#define OP_USBCMD    0x00
#define OP_USBSTS    0x04
#define OP_CRCR      0x18
#define OP_DCBAAP    0x30
#define OP_CONFIG    0x38
#define OP_PORTSC(p) (0x400 + ((p) - 1) * 0x10)

#define CMD_RS       0x1
#define CMD_HCRST    0x2
#define STS_HCH      0x1
#define STS_CNR      0x800

#define PORT_CCS     (1u << 0)
#define PORT_PED     (1u << 1)
#define PORT_PR      (1u << 4)
#define PORT_PP      (1u << 9)
#define PORT_PRC     (1u << 21)
#define PORT_KEEP    (PORT_PP | 3u << 14 | 7u << 25)   /* safe to write back */

/* TRB types */
#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_ENABLE_SLOT  9
#define TRB_ADDRESS      11
#define TRB_CONFIG_EP    12
#define TRB_EVALUATE     13
#define EV_TRANSFER  32
#define EV_COMMAND   33

#define CC_SUCCESS   1
#define CC_SHORT     13

struct ring {
  uint32_t phys;
  uint32_t idx, cycle;
};

struct kbd {                            /* a HID boot interface */
  struct hc *hc;
  int slot, dci;
  int mouse;                            /* else a keyboard */
  struct ring ring;
  uint32_t bufs;                        /* page: NBUF reports, 64 bytes apart */
  uint32_t next;                        /* buffer of the next completion */
};

struct hc {
  volatile uint8_t *cap, *op, *rt;
  volatile uint32_t *db;
  uint32_t csz;                         /* context size: 32 or 64 */
  uint32_t ports, slots;
  uint32_t dcbaa;
  struct ring cmd;
  uint32_t ev_phys, ev_idx, ev_cycle;
  /* last command completion / control transfer event */
  int cmd_done, cmd_cc, cmd_slot;
  int xfer_done, xfer_cc;
  uint32_t xfer_left;
};

static struct hc hcs[MAX_HC];
static struct kbd kbds[MAX_KBD];
static int nhc, nkbd;

static uint32_t r32(volatile uint8_t *base, int r)
{
  return *(volatile uint32_t *)(base + r);
}

static void w32(volatile uint8_t *base, int r, uint32_t v)
{
  *(volatile uint32_t *)(base + r) = v;
}

static void w64(volatile uint8_t *base, int r, uint32_t v)
{
  w32(base, r, v);
  w32(base, r + 4, 0);
}

static int wait32(volatile uint8_t *base, int r, uint32_t mask, uint32_t val, uint32_t ms)
{
  uint32_t i;
  for (i = 0; i < ms * 1000; i++)
  {
    if ((r32(base, r) & mask) == val)
      return 1;
    udelay(1);
  }
  return 0;
}

/* ---------------------------------------------------------------- */
/* rings: one page, 255 TRBs and a link TRB back to the start       */

static int ring_init(struct ring *r)
{
  uint32_t *t;

  if (!(r->phys = page_alloc()))
    return 0;
  t = pool_ptr(r->phys);
  t[255 * 4] = r->phys;
  t[255 * 4 + 3] = TRB_LINK << 10 | 2;  /* toggle cycle */
  r->idx = 0;
  r->cycle = 1;
  return 1;
}

static void ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
  uint32_t *t = (uint32_t *)pool_ptr(r->phys) + r->idx * 4;

  t[0] = d0;
  t[1] = d1;
  t[2] = d2;
  __asm__ volatile("" ::: "memory");
  t[3] = (d3 & ~1u) | r->cycle;         /* hand it to the controller last */
  if (++r->idx == 255)
  {
    uint32_t *link = (uint32_t *)pool_ptr(r->phys) + 255 * 4;
    link[3] = (link[3] & ~1u) | r->cycle;
    r->cycle ^= 1;
    r->idx = 0;
  }
}

/* ---------------------------------------------------------------- */
/* events                                                           */

static void kbd_complete(struct kbd *k, uint32_t cc)
{
  uint32_t buf = k->bufs + (k->next % NBUF) * 64;

  if (cc == CC_SUCCESS || cc == CC_SHORT)
  {
    const uint8_t *r = pool_ptr(buf);
    if (k->mouse)
      mouse_input(r[0] & 7, (int8_t)r[1], (int8_t)r[2]);
    else
      usbkbd_report(r);
  }
  /* give the buffer back */
  ring_push(&k->ring, buf, 0, 8, TRB_NORMAL << 10 | 1 << 5 | 1 << 2);   /* IOC ISP */
  k->next++;
  k->hc->db[k->slot] = (uint32_t)k->dci;
}

/* handle every pending event; returns the number handled */
static int events(struct hc *h)
{
  int n = 0;

  for (;;)
  {
    uint32_t *e = (uint32_t *)pool_ptr(h->ev_phys) + h->ev_idx * 4;
    uint32_t type, cc, slot, ep;
    int i;

    if ((e[3] & 1) != h->ev_cycle)
      break;
    type = (e[3] >> 10) & 0x3f;
    cc = e[2] >> 24;
    slot = e[3] >> 24;
    ep = (e[3] >> 16) & 0x1f;
    if (type == EV_COMMAND)
    {
      h->cmd_done = 1;
      h->cmd_cc = (int)cc;
      h->cmd_slot = (int)slot;
    }
    else if (type == EV_TRANSFER)
    {
      for (i = 0; i < nkbd; i++)
        if (kbds[i].hc == h && kbds[i].slot == (int)slot && kbds[i].dci == (int)ep)
          break;
      if (i < nkbd)
        kbd_complete(&kbds[i], cc);
      else if (ep == 1)
      {
        h->xfer_done = 1;
        h->xfer_cc = (int)cc;
        h->xfer_left = e[2] & 0xffffff;
      }
    }
    if (++h->ev_idx == 256)
    {
      h->ev_idx = 0;
      h->ev_cycle ^= 1;
    }
    w64(h->rt, 0x38, (h->ev_phys + h->ev_idx * 16) | 8);   /* ERDP, clear EHB */
    n++;
  }
  return n;
}

void xhci_poll(void)
{
  int i;
  for (i = 0; i < nhc; i++)
    events(&hcs[i]);
}

static int command(struct hc *h, uint32_t d0, uint32_t d3)
{
  uint32_t i;

  h->cmd_done = 0;
  ring_push(&h->cmd, d0, 0, 0, d3);
  h->db[0] = 0;
  for (i = 0; i < 500000 && !h->cmd_done; i++)   /* 500 ms */
  {
    events(h);
    if (!h->cmd_done)
      udelay(1);
  }
  return h->cmd_done ? h->cmd_cc : -1;
}

/* control transfer on EP0 of slot; data (if any) in the page buf */
static int control(struct hc *h, struct ring *ep0, int slot, uint8_t type, uint8_t req,
                   uint16_t value, uint16_t index, uint16_t len, uint32_t buf)
{
  int in = (type & 0x80) != 0;
  uint32_t i, trt = len ? (in ? 3 : 2) : 0;

  h->xfer_done = 0;
  ring_push(ep0, (uint32_t)type | (uint32_t)req << 8 | (uint32_t)value << 16,
            (uint32_t)index | (uint32_t)len << 16, 8, TRB_SETUP << 10 | 1 << 6 | trt << 16);
  if (len)
    ring_push(ep0, buf, 0, len, TRB_DATA << 10 | (in ? 1u << 16 : 0));
  ring_push(ep0, 0, 0, 0, TRB_STATUS << 10 | 1 << 5 | (len && in ? 0 : 1u << 16));
  h->db[slot] = 1;
  for (i = 0; i < 500000 && !h->xfer_done; i++)
  {
    events(h);
    if (!h->xfer_done)
      udelay(1);
  }
  if (!h->xfer_done)
    return -1;
  return h->xfer_cc == CC_SUCCESS || h->xfer_cc == CC_SHORT ? 0 : h->xfer_cc;
}

/* ---------------------------------------------------------------- */
/* devices                                                          */

static uint32_t *ctx(uint32_t page, struct hc *h, int n)
{
  return (uint32_t *)((uint8_t *)pool_ptr(page) + n * h->csz);
}

/* xHCI interval (2^n x 125 us) from bInterval */
static uint32_t ep_interval(int speed, uint8_t binterval)
{
  uint32_t n = 3, frames = binterval ? binterval : 1;

  if (speed >= 3)                       /* high/super speed: 2^(b-1) */
    return binterval >= 1 && binterval <= 16 ? binterval - 1u : 3;
  while (n < 10 && (1u << (n + 1)) <= frames * 8)
    n++;
  return n;                             /* full/low speed: frames */
}

static void port_device(struct hc *h, uint32_t port)
{
  uint32_t ps = r32(h->op, OP_PORTSC(port)), speed, mps, in, out, buf, cfgval;
  struct ring ep0;
  struct kbd *k;
  uint8_t *b;
  uint32_t *c;
  struct { int ifnum, mouse, ep, mps, interval; } ifs[MAX_IF];
  int slot, cc, off, total, nif = 0, cur = -1, i, n, maxdci;

  if (!(ps & PORT_CCS))
    return;
  if (!(ps & PORT_PED))                 /* USB 2 ports need a reset */
  {
    w32(h->op, OP_PORTSC(port), (ps & PORT_KEEP) | PORT_PR);
    if (!wait32(h->op, OP_PORTSC(port), PORT_PRC, PORT_PRC, 500))
      return;
    ps = r32(h->op, OP_PORTSC(port));
    w32(h->op, OP_PORTSC(port), (ps & PORT_KEEP) | PORT_PRC);
    udelay(20000);                      /* reset recovery */
    ps = r32(h->op, OP_PORTSC(port));
    if (!(ps & PORT_PED))
      return;
  }
  speed = (ps >> 10) & 0xf;
  mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;

  cc = command(h, 0, TRB_ENABLE_SLOT << 10);
  slot = h->cmd_slot;
  if (cc != CC_SUCCESS || slot <= 0 || slot > (int)h->slots)
    return;
  in = page_alloc();
  out = page_alloc();
  buf = page_alloc();
  if (!in || !out || !buf || !ring_init(&ep0))
    return;
  ((uint32_t *)pool_ptr(h->dcbaa))[slot * 2] = out;

  /* address the device: slot context and EP0 */
  c = ctx(in, h, 0);
  c[1] = 3;                             /* add slot, EP0 */
  c = ctx(in, h, 1);
  c[0] = speed << 20 | 1u << 27;
  c[1] = port << 16;
  c = ctx(in, h, 2);
  c[1] = 3u << 1 | 4u << 3 | mps << 16; /* CErr 3, control */
  c[2] = ep0.phys | 1;
  c[4] = 8;
  if ((cc = command(h, in, TRB_ADDRESS << 10 | (uint32_t)slot << 24)) != CC_SUCCESS)
  {
    kprintf("xhci: port %u: address device failed (%d)\n", port, cc);
    return;
  }

  /* device descriptor: max packet size of EP0 */
  b = pool_ptr(buf);
  if (control(h, &ep0, slot, 0x80, 6, 0x0100, 0, 8, buf))
    return;
  if (speed < 4 && b[7] && b[7] != mps)
  {
    mps = b[7];
    memset(pool_ptr(in), 0, 4096);
    ctx(in, h, 0)[1] = 2;               /* evaluate EP0 */
    c = ctx(in, h, 2);
    c[1] = 3u << 1 | 4u << 3 | mps << 16;
    c[2] = ep0.phys | ep0.cycle;
    c[4] = 8;
    command(h, in, TRB_EVALUATE << 10 | (uint32_t)slot << 24);
  }

  /* configuration descriptor: HID boot interfaces (keyboard, mouse) */
  if (control(h, &ep0, slot, 0x80, 6, 0x0200, 0, 9, buf))
    return;
  total = b[2] | b[3] << 8;
  if (total > 4096)
    total = 4096;
  if (control(h, &ep0, slot, 0x80, 6, 0x0200, 0, (uint16_t)total, buf))
    return;
  cfgval = b[5];
  for (off = 0; off + 2 <= total && b[off] >= 2; off += b[off])
  {
    if (b[off + 1] == 4)                /* interface */
    {
      cur = -1;
      if (nif < MAX_IF && b[off + 5] == 3 && b[off + 6] == 1 &&
          (b[off + 7] == 1 || b[off + 7] == 2))
      {
        cur = nif++;
        ifs[cur].ifnum = b[off + 2];
        ifs[cur].mouse = b[off + 7] == 2;
        ifs[cur].ep = 0;
      }
    }
    else if (b[off + 1] == 5 && cur >= 0 && !ifs[cur].ep &&      /* interrupt IN */
             (b[off + 2] & 0x80) && (b[off + 3] & 3) == 3)
    {
      ifs[cur].ep = b[off + 2];
      ifs[cur].mps = (b[off + 4] | b[off + 5] << 8) & 0x7ff;
      ifs[cur].interval = b[off + 6];
    }
  }
  for (i = n = 0; i < nif; i++)         /* keep those with an endpoint */
    if (ifs[i].ep)
      ifs[n++] = ifs[i];
  nif = n;
  if (!nif || nkbd + nif > MAX_KBD)
  {
    kprintf("xhci: port %u: no keyboard or mouse, ignored\n", port);
    return;
  }

  /* configure: boot protocol, report on change only */
  if (control(h, &ep0, slot, 0x00, 9, (uint16_t)cfgval, 0, 0, 0))
    return;
  memset(pool_ptr(in), 0, 4096);
  ctx(in, h, 0)[1] = 1;
  maxdci = 0;
  for (i = 0; i < nif; i++)
  {
    control(h, &ep0, slot, 0x21, 0x0b, 0, (uint16_t)ifs[i].ifnum, 0, 0);  /* SET_PROTOCOL boot */
    control(h, &ep0, slot, 0x21, 0x0a, 0, (uint16_t)ifs[i].ifnum, 0, 0);  /* SET_IDLE 0 */
    k = &kbds[nkbd + i];
    memset(k, 0, sizeof(*k));
    k->hc = h;
    k->slot = slot;
    k->mouse = ifs[i].mouse;
    k->dci = (ifs[i].ep & 0xf) * 2 + 1;
    if (!ring_init(&k->ring) || !(k->bufs = page_alloc()))
      return;
    if (k->dci > maxdci)
      maxdci = k->dci;
    ctx(in, h, 0)[1] |= 1u << k->dci;
    c = ctx(in, h, k->dci + 1);
    c[0] = ep_interval((int)speed, (uint8_t)ifs[i].interval) << 16;
    c[1] = 3u << 1 | 7u << 3 | (uint32_t)ifs[i].mps << 16;   /* CErr 3, interrupt IN */
    c[2] = k->ring.phys | 1;
    c[4] = 8 | (uint32_t)ifs[i].mps << 16;
  }
  c = ctx(in, h, 1);
  c[0] = speed << 20 | (uint32_t)maxdci << 27;
  c[1] = port << 16;
  if ((cc = command(h, in, TRB_CONFIG_EP << 10 | (uint32_t)slot << 24)) != CC_SUCCESS)
  {
    kprintf("xhci: port %u: configure endpoint failed (%d)\n", port, cc);
    return;
  }
  for (i = 0; i < nif; i++)
  {
    k = &kbds[nkbd + i];
    for (off = 0; off < NBUF; off++)
      ring_push(&k->ring, k->bufs + off * 64, 0, 8, TRB_NORMAL << 10 | 1 << 5 | 1 << 2);
    h->db[slot] = (uint32_t)k->dci;
    kprintf("xhci: port %u: %s (speed %u, slot %d)\n", port, k->mouse ? "mouse" : "keyboard",
            speed, slot);
  }
  nkbd += nif;
  page_free(in);
  page_free(buf);
}

/* ---------------------------------------------------------------- */
/* controller                                                       */

static void handoff(volatile uint8_t *cap)
{
  uint32_t off = (r32(cap, CAP_HCC1) >> 16) * 4;
  int guard;

  for (guard = 0; off && guard < 32; guard++)
  {
    uint32_t v = r32(cap, (int)off);
    if ((v & 0xff) == 1)                /* USB legacy support */
    {
      w32(cap, (int)off, v | 1u << 24); /* OS owned */
      if (!wait32(cap, (int)off, 1u << 16, 0, 1000))
        kprintf("xhci: firmware does not release the controller\n");
      v = r32(cap, (int)off + 4);       /* no SMIs, clear their status */
      w32(cap, (int)off + 4, (v & ~0xe01fu) | 0xe0000000u);
      return;
    }
    if (!((v >> 8) & 0xff))
      return;
    off += ((v >> 8) & 0xff) * 4;
  }
}

static void found(int bus, int dev, int fn)
{
  uint64_t bar = pci_bar(bus, dev, fn, 0x10);
  volatile uint8_t *cap;
  struct hc *h;
  uint32_t hcs1, hcs2, need, sp, i, *erst, *dcbaa;

  if (!bar || nhc == MAX_HC)
    return;
  pci_enable(bus, dev, fn);
  if (!(cap = mmio_map(bar, 0x1000)))
    return;
  hcs1 = r32(cap, CAP_HCS1);
  need = r32(cap, CAP_DBOFF) + 4 * 256;
  if (r32(cap, CAP_RTSOFF) + 0x40 > need)
    need = r32(cap, CAP_RTSOFF) + 0x40;
  if ((r32(cap, CAP_LENGTH) & 0xff) + OP_PORTSC((hcs1 >> 24) + 1) > need)
    need = (r32(cap, CAP_LENGTH) & 0xff) + OP_PORTSC((hcs1 >> 24) + 1);
  if (need > 0x1000 && !(cap = mmio_map(bar, need)))
    return;

  h = &hcs[nhc];
  memset(h, 0, sizeof(*h));
  h->cap = cap;
  h->op = cap + (r32(cap, CAP_LENGTH) & 0xff);
  h->rt = cap + (r32(cap, CAP_RTSOFF) & ~0x1fu);
  h->db = (volatile uint32_t *)(cap + (r32(cap, CAP_DBOFF) & ~3u));
  h->csz = r32(cap, CAP_HCC1) & 4 ? 64 : 32;
  h->ports = hcs1 >> 24;
  h->slots = (hcs1 & 0xff) < MAX_SLOTS ? hcs1 & 0xff : MAX_SLOTS;
  kprintf("xhci: controller %02x:%02x.%x, version %x, %u ports\n", bus, dev, fn,
          r32(cap, CAP_LENGTH) >> 16, h->ports);

  handoff(cap);
  w32(h->op, OP_USBCMD, r32(h->op, OP_USBCMD) & ~CMD_RS);
  if (!wait32(h->op, OP_USBSTS, STS_HCH, STS_HCH, 100))
    return;
  w32(h->op, OP_USBCMD, CMD_HCRST);
  if (!wait32(h->op, OP_USBCMD, CMD_HCRST, 0, 1000) ||
      !wait32(h->op, OP_USBSTS, STS_CNR, 0, 1000))
  {
    kprintf("xhci: reset failed\n");
    return;
  }
  w32(h->op, OP_CONFIG, h->slots);

  /* device context array (+ scratchpad buffers), command and event ring */
  if (!(h->dcbaa = page_alloc()) || !ring_init(&h->cmd) || !(h->ev_phys = page_alloc()))
    return;
  dcbaa = pool_ptr(h->dcbaa);
  hcs2 = r32(cap, CAP_HCS2);
  sp = (hcs2 >> 27 & 0x1f) | (hcs2 >> 21 & 0x1f) << 5;
  if (sp)
  {
    uint32_t arr = page_alloc(), *a;
    if (!arr || sp > 512)
      return;
    a = pool_ptr(arr);
    for (i = 0; i < sp; i++)
      if (!(a[i * 2] = page_alloc()))
        return;
    dcbaa[0] = arr;
  }
  w64(h->op, OP_DCBAAP, h->dcbaa);
  w64(h->op, OP_CRCR, h->cmd.phys | 1);
  h->ev_cycle = 1;
  erst = (uint32_t *)pool_ptr(h->dcbaa) + 512;      /* upper half of that page */
  erst[0] = h->ev_phys;
  erst[1] = 0;
  erst[2] = 256;
  w32(h->rt, 0x28, 1);                  /* ERSTSZ */
  w64(h->rt, 0x38, h->ev_phys);         /* ERDP */
  w64(h->rt, 0x30, h->dcbaa + 2048);    /* ERSTBA */
  w32(h->op, OP_USBCMD, CMD_RS);
  if (!wait32(h->op, OP_USBSTS, STS_HCH, 0, 100))
    return;
  nhc++;

  /* power the ports, let connections settle, then set up the devices */
  for (i = 1; i <= h->ports; i++)
  {
    uint32_t ps = r32(h->op, OP_PORTSC(i));
    if (!(ps & PORT_PP))
      w32(h->op, OP_PORTSC(i), (ps & PORT_KEEP) | PORT_PP);
  }
  udelay(200000);
  for (i = 1; i <= h->ports; i++)
    port_device(h, i);
}

void xhci_init(void)
{
  pci_scan(0x0c0330, found);            /* serial bus, USB, xHCI */
}
