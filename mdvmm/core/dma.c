/* MDVMM core - ISA DMA virtualization (8237, both controllers)
 *
 * DMA controllers work with physical addresses, but V86 memory is paged:
 * DOS sessions have private pages below 640K. The monitor keeps a shadow
 * of every channel's address, count and page register and programs the
 * real controller itself:
 *  - target identity mapped (session 0, global memory): the guest's
 *    values, unchanged
 *  - target physically contiguous, below 16MB, no 64K/128K crossing:
 *    the translated address
 *  - otherwise through a bounce buffer below 16MB. Memory->device data
 *    is copied in when the channel is unmasked; device->memory data is
 *    copied out at every hardware interrupt (before any handler runs),
 *    when the channel is masked and when the status register is read.
 *    The target pages are recorded when the transfer starts, so data
 *    lands in the right session even after a switch.
 * Reads of the current address are translated back to the guest's view.
 * Port I/O from DPMI clients (IOPL 3) is not virtualized.
 */
#include "core.h"

#define NCH         8
#define BOUNCE_SIZE 0x20000u            /* 128K: largest 16 bit transfer */
#define MAX_PAGES   (BOUNCE_SIZE / 4096 + 1)

#define MODE_TYPE(m)  (((m) >> 2) & 3)  /* 1: device->memory, 2: memory->device */
#define MODE_AUTO     0x10

#define HAVE_ADDR   1
#define HAVE_COUNT  2
#define HAVE_ALL    (HAVE_ADDR | HAVE_COUNT)

struct chan {
  uint16_t addr, count;                 /* guest values */
  uint8_t page, mode;
  uint8_t masked, have, page_set;
  uint16_t hw_addr;                     /* address programmed into the 8237 */
  /* bounce transfer */
  uint32_t len, copied, off0;
  uint32_t phys[MAX_PAGES];             /* target page frames */
};

static struct chan ch[NCH];
static uint8_t ff[2];                   /* guest view of the flip-flops */
static uint8_t rd_hi[2];                /* high byte of a 16 bit read */
static uint32_t bounce;                 /* physical, 0 if none */
static int bounce_owner = -1;

static const uint16_t page_port[NCH] = { 0x87, 0x83, 0x81, 0x82, 0x8f, 0x8b, 0x89, 0x8a };

/* controller register ports: r = 0 (status/command) .. 7 (write all mask) */
static uint16_t reg_port(int ctl, int r)
{
  return ctl ? 0xd0 + r * 2 : 0x08 + r;
}

static uint16_t addr_port(int c)
{
  return c < 4 ? c * 2 : 0xc0 + (c - 4) * 4;
}

/* port -> kind: 1 address, 2 count, 3 page, 4 + r controller register;
   *c = channel or controller; 0 if not a DMA port */
static int decode(uint16_t port, int *c)
{
  int i;

  if (port < 0x08)
  {
    *c = port >> 1;
    return 1 + (port & 1);
  }
  if (port < 0x10)
  {
    *c = 0;
    return 4 + (port - 0x08);
  }
  if (port >= 0xc0 && port < 0xd0 && !(port & 1))
  {
    *c = 4 + ((port - 0xc0) >> 2);
    return 1 + ((port >> 1) & 1);
  }
  if (port >= 0xd0 && port < 0xe0 && !(port & 1))
  {
    *c = 1;
    return 4 + ((port - 0xd0) >> 1);
  }
  for (i = 0; i < NCH; i++)
    if (port == page_port[i])
    {
      *c = i;
      return 3;
    }
  return 0;
}

int dma_port(uint16_t port)
{
  int c;
  return bounce && decode(port, &c);
}

/* ---------------------------------------------------------------- */
/* real controller                                                  */

static void hw_mask(int c, int mask)
{
  outb(reg_port(c >> 2, 2), (mask ? 4 : 0) | (c & 3));
}

static uint16_t hw_read16(uint16_t port, int ctl)
{
  uint16_t v;

  outb(reg_port(ctl, 4), 0);            /* clear flip-flop */
  v = inb(port);
  v |= inb(port) << 8;
  return v;
}

static void hw_write16(uint16_t port, int ctl, uint16_t v)
{
  outb(reg_port(ctl, 4), 0);
  outb(port, v & 0xff);
  outb(port, v >> 8);
}

/* ---------------------------------------------------------------- */
/* bounce buffer                                                    */

/* copy [a, b) of a bounce transfer; to_guest: bounce -> target pages */
static void bounce_copy(struct chan *k, uint32_t a, uint32_t b, int to_guest)
{
  while (a < b)
  {
    uint32_t o = k->off0 + a;
    uint32_t n = 4096 - (o & 0xfff);
    uint32_t g = k->phys[o >> 12] + (o & 0xfff);

    if (n > b - a)
      n = b - a;
    if (to_guest)
      phys_copy(g, bounce + a, n);
    else
      phys_copy(bounce + a, g, n);
    a += n;
  }
}

/* device->memory: copy what the device has written so far */
static void bounce_flush(int c)
{
  struct chan *k = &ch[c];
  uint32_t unit = c < 4 ? 1 : 2, done;
  uint16_t rem;

  if (MODE_TYPE(k->mode) != 1)
  {
    /* memory->device, auto-init (sound playback): refresh the buffer */
    if (MODE_TYPE(k->mode) == 2 && (k->mode & MODE_AUTO))
      bounce_copy(k, 0, k->len, 0);
    return;
  }
  rem = hw_read16(addr_port(c) + (c < 4 ? 1 : 2), c >> 2);
  done = k->len - (uint32_t)(uint16_t)(rem + 1) * unit;
  if (done >= k->copied)
    bounce_copy(k, k->copied, done, 1);
  else
  {
    bounce_copy(k, k->copied, k->len, 1);   /* auto-init wrapped */
    bounce_copy(k, 0, done, 1);
  }
  k->copied = done;
  if (done == k->len && !(k->mode & MODE_AUTO))
    bounce_owner = -1;                  /* complete */
}

void dma_irq(void)
{
  if (bounce_owner >= 0)
    bounce_flush(bounce_owner);
}

/* ---------------------------------------------------------------- */

/* program the real channel from the shadow */
static void sync(int c)
{
  struct chan *k = &ch[c];
  int wide = c >= 4;
  uint32_t lin, len, p, pte0, phys0, ident = 1, contig = 1;
  uint32_t boundary = wide ? 0x20000 : 0x10000;
  uint16_t hw_count = k->count;
  uint8_t hw_page = k->page;
  static int logged;

  if ((k->have & HAVE_ALL) != HAVE_ALL || !k->page_set)
  {
    hw_mask(c, k->masked);              /* not programmed since we started */
    return;
  }
  lin = wide ? ((uint32_t)(k->page & 0xfe) << 16) | ((uint32_t)k->addr << 1)
             : ((uint32_t)k->page << 16) | k->addr;
  len = ((uint32_t)k->count + 1) * (wide ? 2 : 1);
  k->hw_addr = k->addr;

  if (bounce_owner == c)
  {
    bounce_flush(c);
    bounce_owner = -1;
  }

  pte0 = lin_to_pte(lin);
  phys0 = (pte0 & ~0xfffu) | (lin & 0xfff);
  for (p = lin & ~0xfffu; p < lin + len; p += 4096)
  {
    uint32_t e = lin_to_pte(p);
    if (!(e & 1))
    {
      ident = 1;                        /* nothing sensible: leave it raw */
      contig = 1;
      phys0 = lin;
      break;
    }
    if ((e & ~0xfffu) != p)
      ident = 0;
    if ((e & ~0xfffu) != (pte0 & ~0xfffu) + (p - (lin & ~0xfffu)))
      contig = 0;
  }

  if (!ident)
  {
    uint32_t phys = phys0;

    if (!contig || phys + len > 0x1000000 || phys / boundary != (phys + len - 1) / boundary)
    {
      /* bounce */
      if (bounce_owner >= 0 && bounce_owner != c)
      {
        bounce_flush(bounce_owner);
        kprintf("dma: channel %d takes the bounce buffer from %d\n", c, bounce_owner);
      }
      if (len > BOUNCE_SIZE)
        len = BOUNCE_SIZE;
      k->len = len;
      k->copied = 0;
      k->off0 = lin & 0xfff;
      for (p = 0; p * 4096 < k->off0 + len; p++)
        k->phys[p] = lin_to_pte((lin & ~0xfffu) + p * 4096) & ~0xfffu;
      if (MODE_TYPE(k->mode) == 2)
        bounce_copy(k, 0, len, 0);
      bounce_owner = c;
      phys = bounce;
      if (!logged)
      {
        kprintf("dma: channel %d uses the bounce buffer (%u bytes at %05x)\n", c, len, lin);
        logged = 1;
      }
    }
    hw_page = (uint8_t)(phys >> 16);
    k->hw_addr = wide ? (uint16_t)(phys >> 1) : (uint16_t)phys;
  }

  hw_mask(c, 1);
  hw_write16(addr_port(c), c >> 2, k->hw_addr);
  hw_write16(addr_port(c) + (wide ? 2 : 1), c >> 2, hw_count);
  outb(page_port[c], hw_page);
  outb(reg_port(c >> 2, 3), k->mode);
  if (!k->masked)
    hw_mask(c, 0);
}

static void set_mask(int c, int mask)
{
  if (mask && bounce_owner == c)
    bounce_flush(c);
  ch[c].masked = (uint8_t)mask;
}

/* ---------------------------------------------------------------- */
/* guest port access                                                */

uint8_t dma_in(uint16_t port)
{
  int c, kind = decode(port, &c), ctl, i;
  uint16_t v;

  switch (kind)
  {
    case 1:                             /* current address */
    case 2:                             /* current count */
      ctl = c >> 2;
      if (ff[ctl])
      {
        ff[ctl] = 0;
        return rd_hi[ctl];
      }
      v = hw_read16(port, ctl);
      if (kind == 1)
        v = (uint16_t)(v - ch[c].hw_addr + ch[c].addr);
      ff[ctl] = 1;
      rd_hi[ctl] = v >> 8;
      return v & 0xff;
    case 3:
      return ch[c].page_set ? ch[c].page : inb(port);
    case 4:                             /* status: TC bits, flush first */
      for (i = 0; i < 4; i++)
        if (bounce_owner == c * 4 + i)
          bounce_flush(bounce_owner);
      break;
  }
  return inb(port);
}

void dma_out(uint16_t port, uint8_t v)
{
  int c, kind = decode(port, &c), i;
  struct chan *k;

  switch (kind)
  {
    case 1:
    case 2:
    {
      int ctl = c >> 2;
      uint16_t *r = kind == 1 ? &ch[c].addr : &ch[c].count;
      if (ff[ctl])
        *r = (uint16_t)((*r & 0xff) | v << 8);
      else
        *r = (uint16_t)((*r & 0xff00) | v);
      ff[ctl] ^= 1;
      ch[c].have |= kind == 1 ? HAVE_ADDR : HAVE_COUNT;
      if (!ch[c].masked)
        sync(c);
      return;
    }
    case 3:
      ch[c].page = v;
      ch[c].page_set = 1;
      if (!ch[c].masked)
        sync(c);
      return;
    case 4 + 2:                         /* single mask */
      i = c * 4 + (v & 3);
      set_mask(i, v & 4);
      if (v & 4)
        break;
      sync(i);
      return;
    case 4 + 3:                         /* mode */
      ch[c * 4 + (v & 3)].mode = v;
      break;
    case 4 + 4:                         /* clear flip-flop */
      ff[c] = 0;
      return;
    case 4 + 5:                         /* master clear */
      ff[c] = 0;
      for (i = c * 4; i < c * 4 + 4; i++)
      {
        set_mask(i, 1);
        if (bounce_owner == i)
          bounce_owner = -1;
      }
      break;
    case 4 + 6:                         /* clear all masks */
      for (i = c * 4; i < c * 4 + 4; i++)
      {
        set_mask(i, 0);
        sync(i);
      }
      break;
    case 4 + 7:                         /* write all masks */
      for (i = 0; i < 4; i++)
      {
        k = &ch[c * 4 + i];
        set_mask(c * 4 + i, (v >> i) & 1);
        if (!k->masked)
          sync(c * 4 + i);
      }
      break;
  }
  outb(port, v);
}

void dma_init(void)
{
  int i;
  uint16_t p;

  for (i = 0; i < NCH; i++)
    ch[i].masked = i != 4;              /* channel 4 cascades */
  bounce = page_alloc_isa(BOUNCE_SIZE);
  if (!bounce)
  {
    kprintf("dma: no memory below 16MB for a bounce buffer\n");
    return;
  }
  for (p = 0; p < 0x100; p++)
  {
    int c;
    if (decode(p, &c))
      io_trap(p, 1);
  }
  kprintf("dma: bounce buffer at %08x\n", bounce);
}
