/* MDVMM core - Intel e1000 family network driver (8254x, 82574)
 *
 * Legacy descriptors: 32 receive buffers of 2048 bytes, 16 transmit
 * buffers; polled, interrupts masked. The MAC comes from receive address
 * 0 (set by the firmware or QEMU), else from the EEPROM. QEMU's default
 * NIC (82540EM) and the common desktop/server chips of the family.
 */
#include "core.h"

#define R_CTRL   0x0000
#define R_STATUS 0x0008
#define R_EERD   0x0014
#define R_ICR    0x00c0
#define R_IMC    0x00d8
#define R_RCTL   0x0100
#define R_TCTL   0x0400
#define R_TIPG   0x0410
#define R_RDBAL  0x2800
#define R_RDBAH  0x2804
#define R_RDLEN  0x2808
#define R_RDH    0x2810
#define R_RDT    0x2818
#define R_TDBAL  0x3800
#define R_TDBAH  0x3804
#define R_TDLEN  0x3808
#define R_TDH    0x3810
#define R_TDT    0x3818
#define R_MTA    0x5200
#define R_RAL    0x5400
#define R_RAH    0x5404

#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)
#define CTRL_RST   (1u << 26)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

#define NRX 32
#define NTX 16
#define BUF 2048

struct rx_desc { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; };
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; };

struct e1000 {
  struct nic nic;
  volatile uint8_t *regs;
  struct rx_desc *rx;
  struct tx_desc *tx;
  uint32_t rx_buf[NRX], tx_buf[NTX];    /* physical */
  int rx_next, tx_next;
};

static struct e1000 cards[2];
static int ncards;

static const uint16_t ids[] = {
  0x100e, 0x100f, 0x1004, 0x1008, 0x1009, 0x100c, 0x100d, 0x1010, 0x1011, 0x1012, 0x1013,
  0x1015, 0x1016, 0x1017, 0x1018, 0x1019, 0x101a, 0x101d, 0x101e, 0x1026, 0x1027, 0x1028,
  0x1075, 0x1076, 0x1077, 0x1078, 0x1079, 0x107a, 0x107b, 0x107c, 0x108a, 0x1099, 0x10b5,
  0x10d3, 0x10f6, 0x150c,
};

static uint32_t rd(struct e1000 *c, int r) { return *(volatile uint32_t *)(c->regs + r); }
static void wr(struct e1000 *c, int r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }

static void *buf_ptr(uint32_t phys)
{
  return (uint8_t *)pool_ptr(phys & ~0xfffu) + (phys & 0xfff);
}

static int e_send(struct nic *n, const void *data, int len)
{
  struct e1000 *c = (struct e1000 *)n;
  struct tx_desc *d = &c->tx[c->tx_next];
  uint32_t i;

  for (i = 0; i < 100000 && d->cmd && !(d->status & 1); i++)   /* still sending */
    udelay(1);
  if (d->cmd && !(d->status & 1))
    return -1;
  memcpy(buf_ptr(c->tx_buf[c->tx_next]), data, len);
  d->addr = c->tx_buf[c->tx_next];
  d->len = (uint16_t)(len < 60 ? 60 : len);  /* the card pads, but be sure */
  d->cso = d->css = 0;
  d->status = 0;
  d->special = 0;
  d->cmd = 0x0b;                        /* EOP, IFCS, RS */
  if (len < 60)
    memset((uint8_t *)buf_ptr(c->tx_buf[c->tx_next]) + len, 0, 60 - len);
  c->tx_next = (c->tx_next + 1) % NTX;
  __asm__ volatile("" ::: "memory");
  wr(c, R_TDT, (uint32_t)c->tx_next);
  return 0;
}

static int e_recv(struct nic *n, void *data, int room)
{
  struct e1000 *c = (struct e1000 *)n;
  struct rx_desc *d = &c->rx[c->rx_next];
  int len;

  if (!(d->status & 1))                 /* DD: nothing new */
    return 0;
  len = d->len;
  if (!(d->status & 2) || d->errors)    /* not EOP, or bad: drop */
    len = 0;
  if (len > room)
    len = 0;
  if (len)
    memcpy(data, buf_ptr(c->rx_buf[c->rx_next]), len);
  d->status = 0;
  wr(c, R_RDT, (uint32_t)c->rx_next);   /* give the buffer back */
  c->rx_next = (c->rx_next + 1) % NRX;
  return len ? len : -1;
}

static int e_link(struct nic *n)
{
  return (rd((struct e1000 *)n, R_STATUS) & 2) != 0;
}

static uint16_t eeprom(struct e1000 *c, int addr)
{
  int i;

  wr(c, R_EERD, (uint32_t)addr << 8 | 1);
  for (i = 0; i < 10000; i++)
  {
    uint32_t v = rd(c, R_EERD);
    if (v & 0x10)
      return (uint16_t)(v >> 16);
    udelay(1);
  }
  return 0xffff;
}

static void found(int bus, int dev, int fn)
{
  uint32_t id = pci_read32(bus, dev, fn, 0);
  uint64_t bar = pci_bar(bus, dev, fn, 0x10);
  struct e1000 *c;
  uint32_t ral, rah, ring, i;
  unsigned k;

  if ((id & 0xffff) != 0x8086 || ncards == 2 || !bar)
    return;
  for (k = 0; k < sizeof(ids) / sizeof(ids[0]) && ids[k] != id >> 16; k++)
    ;
  if (k == sizeof(ids) / sizeof(ids[0]))
    return;
  c = &cards[ncards];
  memset(c, 0, sizeof(*c));
  pci_enable(bus, dev, fn);
  if (!(c->regs = mmio_map(bar, 0x20000)))
    return;

  wr(c, R_IMC, 0xffffffffu);
  wr(c, R_CTRL, rd(c, R_CTRL) | CTRL_RST);
  udelay(10000);
  wr(c, R_IMC, 0xffffffffu);
  rd(c, R_ICR);
  wr(c, R_CTRL, (rd(c, R_CTRL) | CTRL_SLU | CTRL_ASDE) & ~((1u << 3) | (1u << 7) | (1u << 31)));

  ral = rd(c, R_RAL);
  rah = rd(c, R_RAH);
  if (rah & 0x80000000u)
  {
    for (i = 0; i < 4; i++)
      c->nic.mac[i] = (uint8_t)(ral >> (i * 8));
    c->nic.mac[4] = (uint8_t)rah;
    c->nic.mac[5] = (uint8_t)(rah >> 8);
  }
  else
  {
    for (i = 0; i < 3; i++)
    {
      uint16_t w = eeprom(c, (int)i);
      c->nic.mac[i * 2] = (uint8_t)w;
      c->nic.mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    wr(c, R_RAL, c->nic.mac[0] | c->nic.mac[1] << 8 | c->nic.mac[2] << 16 |
                 (uint32_t)c->nic.mac[3] << 24);
    wr(c, R_RAH, c->nic.mac[4] | c->nic.mac[5] << 8 | 0x80000000u);
  }
  for (i = 0; i < 128; i++)
    wr(c, R_MTA + i * 4, 0);

  /* rings: one page each, buffers two per page */
  if (!(ring = page_alloc()))
    return;
  c->rx = pool_ptr(ring);
  for (i = 0; i < NRX; i++)
  {
    uint32_t p = i & 1 ? c->rx_buf[i - 1] + BUF : page_alloc();
    if (!p)
      return;
    c->rx_buf[i] = p;
    c->rx[i].addr = p;
    c->rx[i].status = 0;
  }
  wr(c, R_RDBAL, ring);
  wr(c, R_RDBAH, 0);
  wr(c, R_RDLEN, NRX * 16);
  wr(c, R_RDH, 0);
  wr(c, R_RDT, NRX - 1);
  wr(c, R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* 2048-byte buffers */

  if (!(ring = page_alloc()))
    return;
  c->tx = pool_ptr(ring);
  for (i = 0; i < NTX; i++)
  {
    uint32_t p = i & 1 ? c->tx_buf[i - 1] + BUF : page_alloc();
    if (!p)
      return;
    c->tx_buf[i] = p;
  }
  wr(c, R_TDBAL, ring);
  wr(c, R_TDBAH, 0);
  wr(c, R_TDLEN, NTX * 16);
  wr(c, R_TDH, 0);
  wr(c, R_TDT, 0);
  wr(c, R_TIPG, 0x0060200a);
  wr(c, R_TCTL, TCTL_EN | TCTL_PSP | 0x10u << 4 | 0x40u << 12);

  c->nic.name = "Intel e1000";
  c->nic.send = e_send;
  c->nic.recv = e_recv;
  c->nic.link = e_link;
  ncards++;
  kprintf("e1000: %02x:%02x.%x device %04x, link %s\n", bus, dev, fn, id >> 16,
          e_link(&c->nic) ? "up" : "down");
  nic_register(&c->nic);
}

void e1000_init(void)
{
  pci_scan(0x020000, found);            /* network, Ethernet */
}
