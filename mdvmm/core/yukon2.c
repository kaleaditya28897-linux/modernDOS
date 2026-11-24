/* MDVMM core - Marvell Yukon-2 network driver (88E8058 and relatives)
 *
 * The PCI Express gigabit chips of 2005-2010 (88E8036 ... 88E8058; the
 * Ethernet of Intel Macs, many notebooks and desktop boards), one port,
 * polled, no interrupts. Written after the register descriptions in
 * Linux's sky2 driver (sky2.h); QEMU has no model of the chip, so it is
 * only tested on real hardware (an iMac8,1, 88E8058).
 *
 * The chip works with "list elements" of 8 bytes: a receive ring of
 * buffers, a transmit ring of frames, and a status ring where the chip
 * reports received frames (in buffer order) and transmit progress.
 */
#include "core.h"

/* control */
#define B0_CTST         0x0004
#define B0_POWER_CTRL   0x0007
#define B0_IMSK         0x000c
#define B0_HWE_IMSK     0x0014
#define B2_MAC_1        0x0100
#define B2_MAC_CFG      0x011a
#define B2_CHIP_ID      0x011b
#define B2_E_0          0x011c          /* RAM buffer size, 4 KB units */
#define B2_Y2_CLK_GATE  0x011d
#define B2_Y2_CLK_CTRL  0x0120
#define B2_TI_CTRL      0x0138
#define B2_TST_CTRL1    0x0158
#define B2_GP_IO        0x015c
#define B3_RI_WTO_R1    0x0190
#define B3_RI_CTRL      0x01a0
#define TXA_CTRL        0x0210
#define Y2_CFG_SPC      0x1c00          /* PCI configuration space mirror */

#define CS_RST_SET      0x0001
#define CS_RST_CLR      0x0002
#define CS_MRST_CLR     0x0008
#define Y2_LED_STAT_ON  0x0200
#define Y2_HW_WOL_ON    0x8000
#define TST_CFG_WRITE_ON  0x02
#define TST_CFG_WRITE_OFF 0x01

#define PCI_DEV_REG1    0x40
#define PCI_DEV_REG3    0x80
#define PCI_DEV_REG4    0x84
#define PCI_DEV_REG5    0x88
#define PCI_CFG_REG_1   0x94
#define PCI_Y2_PHY_DOWN (0xfu << 26)    /* power down and coma, both PHYs */

/* queues (BMU), prefetch units, RAM buffers */
#define Q_R1            0x0000
#define Q_XS1           0x0200
#define Q_XA1           0x0280
#define Q_CSR           0x0434          /* + queue */
#define Q_TEST          0x0438
#define Q_WM            0x0440
#define Q_AL            0x0442
#define BMU_CLR_RESET   0x0016          /* FIFO reset, operation off, reset clear */
#define BMU_OPER_INIT   0x0d28          /* clear IRQs, start, FIFO enable, operation on */
#define BMU_FIFO_OP_ON  0x0080
#define BMU_DIS_RX_CHKSUM 0x1000
#define BMU_DIS_RX_RSS  0x4000
#define BMU_WM_DEFAULT  0x600
#define BMU_WM_PEX      0x80
#define F_TX_CHK_AUTO_OFF (1u << 31)
#define F_M_RX_RAM_DIS  (1u << 24)

#define PREF_CTRL       0x0450          /* + queue */
#define PREF_LAST_IDX   0x0454
#define PREF_ADDR_LO    0x0458
#define PREF_ADDR_HI    0x045c
#define PREF_GET_IDX    0x0460
#define PREF_PUT_IDX    0x0464
#define PREF_RST_SET    1
#define PREF_RST_CLR    2
#define PREF_OP_ON      8

#define RB_START        0x0800          /* + queue */
#define RB_END          0x0804
#define RB_WP           0x0808
#define RB_RP           0x080c
#define RB_RX_UTPP      0x0810
#define RB_RX_LTPP      0x0814
#define RB_CTRL         0x0828
#define RB_RST_SET      0x01
#define RB_RST_CLR      0x02
#define RB_ENA_OP_MD    0x08
#define RB_ENA_STFWD    0x20

/* MAC FIFOs, GMAC and PHY control */
#define RX_GMF_CTRL_T   0x0c48
#define RX_GMF_FL_MSK   0x0c4c
#define RX_GMF_FL_THR   0x0c50
#define RX_GMF_UP_THR   0x0c58
#define RX_GMF_LP_THR   0x0c5a
#define TX_GMF_CTRL_T   0x0d48
#define GMF_RST_CLR     0x02
#define GMF_OPER_ON     0x08
#define GMF_RX_F_FL_ON  0x80
#define RX_TRUNC_OFF    (1u << 26)
#define TX_STFW_ENA     (1u << 30)
#define GMR_FS_ANY_ERR  0x187b
#define GMR_FS_RX_OK    0x0100

#define GMAC_CTRL       0x0f00
#define GPHY_CTRL       0x0f04
#define GMAC_IRQ_SRC    0x0f08
#define GMAC_IRQ_MSK    0x0f0c
#define GMAC_LINK_CTRL  0x0f10
#define GMC_RST_CLR     0x02
#define GMC_PAUSE_OFF   0x04
#define GPC_RST_SET     0x01
#define GPC_RST_CLR     0x02
#define GMLC_RST_SET    0x01
#define GMLC_RST_CLR    0x02

/* status unit */
#define STAT_CTRL       0x0e80
#define STAT_LAST_IDX   0x0e84
#define STAT_LIST_LO    0x0e88
#define STAT_LIST_HI    0x0e8c
#define STAT_TX_IDX_TH  0x0e98
#define STAT_PUT_IDX    0x0e9c
#define STAT_FIFO_WM    0x0eac
#define STAT_FIFO_ISR_WM 0x0ead
#define STAT_LEV_TIMER_INI  0x0eb0
#define STAT_LEV_TIMER_CTRL 0x0eb8
#define STAT_TX_TIMER_INI   0x0ec0
#define STAT_TX_TIMER_CTRL  0x0ec8
#define STAT_ISR_TIMER_INI  0x0ed0
#define STAT_ISR_TIMER_CTRL 0x0ed8
#define SC_STAT_RST_SET 0x01
#define SC_STAT_RST_CLR 0x02
#define SC_STAT_OP_ON   0x08
#define SC_STAT_CLR_IRQ 0x10
#define TIM_START       0x04
#define TIM_STOP        0x02
#define TIM_CLR_IRQ     0x01

/* GMAC (16 bit registers) */
#define GMAC            0x2800
#define GM_GP_STAT      0x00
#define GM_GP_CTRL      0x04
#define GM_TX_CTRL      0x08
#define GM_RX_CTRL      0x0c
#define GM_TX_FLOW_CTRL 0x10
#define GM_TX_PARAM     0x14
#define GM_SERIAL_MODE  0x18
#define GM_SRC_ADDR_1L  0x1c
#define GM_SRC_ADDR_2L  0x28
#define GM_MC_ADDR_H1   0x34
#define GM_TX_IRQ_MSK   0x50
#define GM_RX_IRQ_MSK   0x54
#define GM_TR_IRQ_MSK   0x58
#define GM_SMI_CTRL     0x80
#define GM_SMI_DATA     0x84
#define GM_GPCR_TX_ENA  0x1000
#define GM_GPCR_RX_ENA  0x0800
#define GM_RXCR_UCF_ENA 0x8000
#define GM_RXCR_CRC_DIS 0x2000
#define GM_SMI_OP_RD    0x20
#define GM_SMI_RD_VAL   0x10
#define GM_SMI_BUSY     0x08

/* list elements */
#define OP_PACKET       0x41
#define OP_RXSTAT       0x60
#define OP_TXINDEXLE    0x68
#define HW_OWNER        0x80
#define LE_EOP          0x80

struct le {
  uint32_t addr;                        /* status ring: status */
  uint16_t length;
  uint8_t ctrl, opcode;
};

#define NRX     32                      /* receive buffers */
#define RX_LE   64                      /* receive ring elements (2^n) */
#define NTX     16
#define TX_LE   32
#define ST_LE   512                     /* status ring: one page */
#define BUF     2048

struct yukon {
  struct nic nic;
  volatile uint8_t *regs;
  uint8_t chip, rev;
  struct le *rx, *tx, *st;
  uint32_t rx_phys, tx_phys, st_phys;
  uint32_t rx_buf[NRX], tx_buf[NTX];    /* physical */
  uint32_t rx_put, rx_next;             /* ring index, buffer of the next frame */
  uint32_t tx_prod, tx_done;            /* ring indexes */
  uint32_t st_idx;
  uint32_t rx_frames, rx_bad, tx_frames, tx_full;
};

static struct yukon card;
static int have_card;

static uint8_t r8(struct yukon *y, uint32_t r) { return y->regs[r]; }
static uint16_t r16(struct yukon *y, uint32_t r) { return *(volatile uint16_t *)(y->regs + r); }
static uint32_t r32(struct yukon *y, uint32_t r) { return *(volatile uint32_t *)(y->regs + r); }
static void w8(struct yukon *y, uint32_t r, uint8_t v) { y->regs[r] = v; }
static void w16(struct yukon *y, uint32_t r, uint16_t v) { *(volatile uint16_t *)(y->regs + r) = v; }
static void w32(struct yukon *y, uint32_t r, uint32_t v) { *(volatile uint32_t *)(y->regs + r) = v; }

static uint16_t gm_read(struct yukon *y, uint32_t r) { return r16(y, GMAC + r); }
static void gm_write(struct yukon *y, uint32_t r, uint16_t v) { w16(y, GMAC + r, v); }

static int phy_read(struct yukon *y, int reg)
{
  int i;

  gm_write(y, GM_SMI_CTRL, (uint16_t)(reg << 6 | GM_SMI_OP_RD));     /* PHY address 0 */
  for (i = 0; i < 2000; i++)
  {
    if (gm_read(y, GM_SMI_CTRL) & GM_SMI_RD_VAL)
      return gm_read(y, GM_SMI_DATA);
    udelay(10);
  }
  return -1;
}

static void phy_write(struct yukon *y, int reg, uint16_t v)
{
  int i;

  gm_write(y, GM_SMI_DATA, v);
  gm_write(y, GM_SMI_CTRL, (uint16_t)(reg << 6));
  for (i = 0; i < 2000 && (gm_read(y, GM_SMI_CTRL) & GM_SMI_BUSY); i++)
    udelay(10);
}

static int y_link(struct nic *n)
{
  struct yukon *y = (struct yukon *)n;

  phy_read(y, 1);                       /* the link bit latches low */
  return (phy_read(y, 1) & 4) != 0;
}

/* RAM buffer of a queue (chips that have one): start and size in KB */
static void ramset(struct yukon *y, uint32_t q, uint32_t start, uint32_t space)
{
  uint32_t end;

  start *= 1024 / 8;
  space *= 1024 / 8;
  end = start + space - 1;
  w8(y, RB_CTRL + q, RB_RST_CLR);
  w32(y, RB_START + q, start);
  w32(y, RB_END + q, end);
  w32(y, RB_WP + q, start);
  w32(y, RB_RP + q, start);
  if (q == Q_R1)
  {
    w32(y, RB_RX_UTPP + q, space - space / 4);
    w32(y, RB_RX_LTPP + q, space / 4);
  }
  else
    w8(y, RB_CTRL + q, RB_ENA_STFWD);
  w8(y, RB_CTRL + q, RB_ENA_OP_MD);
  r8(y, RB_CTRL + q);
}

static void qset(struct yukon *y, uint32_t q)
{
  w32(y, Q_CSR + q, BMU_CLR_RESET);
  w32(y, Q_CSR + q, BMU_OPER_INIT);
  w32(y, Q_CSR + q, BMU_FIFO_OP_ON);
  w32(y, Q_WM + q, BMU_WM_DEFAULT);
}

static void prefetch_init(struct yukon *y, uint32_t q, uint32_t phys, uint32_t last)
{
  w32(y, PREF_CTRL + q, PREF_RST_SET);
  w32(y, PREF_CTRL + q, PREF_RST_CLR);
  w32(y, PREF_ADDR_HI + q, 0);
  w32(y, PREF_ADDR_LO + q, phys);
  w16(y, PREF_LAST_IDX + q, (uint16_t)last);
  w32(y, PREF_CTRL + q, PREF_OP_ON);
  r32(y, PREF_CTRL + q);
}

/* give receive buffer `b` (back) to the chip */
static void rx_add(struct yukon *y, uint32_t b)
{
  struct le *le = &y->rx[y->rx_put];

  le->addr = y->rx_buf[b];
  le->length = BUF;
  le->ctrl = 0;
  le->opcode = OP_PACKET | HW_OWNER;
  y->rx_put = (y->rx_put + 1) % RX_LE;
}

/* the next status element, 0 if there is none */
static struct le *status_next(struct yukon *y)
{
  struct le *le;

  if (y->st_idx == r16(y, STAT_PUT_IDX))
    return 0;
  le = &y->st[y->st_idx];
  if (!(le->opcode & HW_OWNER))
    return 0;
  y->st_idx = (y->st_idx + 1) % ST_LE;
  return le;
}

static void status_tx(struct yukon *y, struct le *le)
{
  y->tx_done = le->addr & 0xfff;        /* port 1's transmit index */
}

static int y_recv(struct nic *n, void *frame, int room)
{
  struct yukon *y = (struct yukon *)n;
  struct le *le;
  int len = 0;

  while (!len && (le = status_next(y)))
  {
    uint8_t op = le->opcode & ~HW_OWNER;
    le->opcode = 0;
    if (op == OP_TXINDEXLE)
      status_tx(y, le);
    else if (op == OP_RXSTAT)
    {
      uint32_t st = le->addr, b = y->rx_next;
      int l = le->length;
      if ((st & GMR_FS_RX_OK) && !(st & GMR_FS_ANY_ERR) && l >= 14 && l <= 1514 && l <= room)
      {
        memcpy(frame, pool_ptr(y->rx_buf[b]), (size_t)l);
        len = l;
        y->rx_frames++;
      }
      else
        y->rx_bad++;
      y->rx_next = (b + 1) % NRX;
      rx_add(y, b);                     /* the buffer goes round again */
      w16(y, PREF_PUT_IDX + Q_R1, (uint16_t)y->rx_put);
    }
  }
  if (!le)
    w32(y, STAT_CTRL, SC_STAT_CLR_IRQ); /* all read */
  return len;
}

static int y_send(struct nic *n, const void *frame, int len)
{
  struct yukon *y = (struct yukon *)n;
  uint32_t next = (y->tx_prod + 1) % TX_LE, slot = y->tx_prod % NTX;
  struct le *le;
  int i;

  /* room: at most NTX frames between done and prod */
  for (i = 0; (y->tx_prod + TX_LE - y->tx_done) % TX_LE >= NTX - 1; i++)
  {
    /* transmit progress only: frames stay queued for y_recv */
    y->tx_done = r16(y, PREF_GET_IDX + Q_XA1) % TX_LE;
    if (i == 20000)
    {
      y->tx_full++;
      return -1;
    }
    udelay(10);
  }
  memcpy(pool_ptr(y->tx_buf[slot]), frame, (size_t)len);
  if (len < 60)
  {
    memset((uint8_t *)pool_ptr(y->tx_buf[slot]) + len, 0, (size_t)(60 - len));
    len = 60;
  }
  le = &y->tx[y->tx_prod];
  le->addr = y->tx_buf[slot];
  le->length = (uint16_t)len;
  le->ctrl = LE_EOP;
  le->opcode = OP_PACKET | HW_OWNER;
  y->tx_prod = next;
  w16(y, PREF_PUT_IDX + Q_XA1, (uint16_t)y->tx_prod);
  y->tx_frames++;
  return 0;
}

static void y_debug(struct nic *n)
{
  struct yukon *y = (struct yukon *)n;

  kprintf("yukon2: chip %02x rev %x, link %s, PHY status %04x, GMAC status %04x control %04x\n",
          y->chip, y->rev, y_link(n) ? "up" : "down", phy_read(y, 17), gm_read(y, GM_GP_STAT),
          gm_read(y, GM_GP_CTRL));
  kprintf("yukon2: rx %u frames %u bad, put %u get %u; tx %u frames %u full, prod %u done %u get %u\n",
          y->rx_frames, y->rx_bad, y->rx_put, r16(y, PREF_GET_IDX + Q_R1), y->tx_frames,
          y->tx_full, y->tx_prod, y->tx_done, r16(y, PREF_GET_IDX + Q_XA1));
  kprintf("yukon2: status ring at %u, chip at %u; queue CSR rx %08x tx %08x; RAM %u KB\n",
          y->st_idx, r16(y, STAT_PUT_IDX), r32(y, Q_CSR + Q_R1), r32(y, Q_CSR + Q_XA1),
          r8(y, B2_E_0) * 4);
}

static uint32_t cfg_read(struct yukon *y, int reg)
{
  return r32(y, Y2_CFG_SPC + (uint32_t)reg);
}

static void cfg_write(struct yukon *y, int reg, uint32_t v)
{
  w32(y, Y2_CFG_SPC + (uint32_t)reg, v);
}

static int alloc_rings(struct yukon *y)
{
  int i;

  if (!(y->rx_phys = page_alloc()) || !(y->tx_phys = page_alloc()) || !(y->st_phys = page_alloc()))
    return 0;
  y->rx = pool_ptr(y->rx_phys);
  y->tx = pool_ptr(y->tx_phys);
  y->st = pool_ptr(y->st_phys);
  for (i = 0; i < NRX; i += 2)
  {
    if (!(y->rx_buf[i] = page_alloc()))
      return 0;
    y->rx_buf[i + 1] = y->rx_buf[i] + BUF;
  }
  for (i = 0; i < NTX; i += 2)
  {
    if (!(y->tx_buf[i] = page_alloc()))
      return 0;
    y->tx_buf[i + 1] = y->tx_buf[i] + BUF;
  }
  return 1;
}

static void found(int bus, int dev, int fn)
{
  struct yukon *y = &card;
  uint32_t id = pci_read32(bus, dev, fn, 0), ram, v;
  uint16_t mac16;
  int i;

  if (have_card || (id & 0xffff) != 0x11ab)
    return;
  if ((id >> 16) < 0x4340 || (id >> 16) > 0x43a0)
    return;                             /* not a Yukon-2 */
  memset(y, 0, sizeof(*y));
  pci_enable(bus, dev, fn);
  y->regs = mmio_map(pci_bar(bus, dev, fn, 0x10), 0x4000);
  if (!y->regs)
    return;
  w8(y, B0_CTST, CS_RST_CLR);
  y->chip = r8(y, B2_CHIP_ID);
  y->rev = (r8(y, B2_MAC_CFG) >> 4) & 0xf;
  kprintf("yukon2: %02x:%02x.%x device %04x, chip %02x rev %x\n", bus, dev, fn, id >> 16, y->chip,
          y->rev);
  if (y->chip < 0xb3 || y->chip > 0xbd)
  {
    kprintf("yukon2: not a Yukon-2 chip, left alone\n");
    return;
  }
  if (!alloc_rings(y))
    return;

  /* reset; power for the chip and the PHY */
  w32(y, B0_IMSK, 0);
  w8(y, B0_CTST, CS_RST_SET);
  w8(y, B0_CTST, CS_RST_CLR);
  w8(y, B2_TST_CTRL1, TST_CFG_WRITE_ON);
  cfg_write(y, 0x04, cfg_read(y, 0x04) | 0xf9000000u);   /* clear PCI error bits */
  w8(y, B0_CTST, CS_MRST_CLR);
  w8(y, B0_POWER_CTRL, 0xa6);           /* VAUX and VCC enabled, VAUX off, VCC on */
  w32(y, B2_Y2_CLK_CTRL, 1);            /* no core clock division */
  w8(y, B2_Y2_CLK_GATE, 0);
  if (y->chip == 0xb4 || y->chip == 0xb5 || y->chip >= 0xb8)   /* advanced power control */
  {
    cfg_write(y, PCI_DEV_REG3, 0);
    cfg_write(y, PCI_DEV_REG4, cfg_read(y, PCI_DEV_REG4) & 0xf000);
    cfg_write(y, PCI_DEV_REG5, cfg_read(y, PCI_DEV_REG5) & (3u << 27));
    cfg_write(y, PCI_CFG_REG_1, 0);
    w16(y, B0_CTST, Y2_HW_WOL_ON);
    w32(y, B2_GP_IO, r32(y, B2_GP_IO) | (1u << 13));     /* no status race */
    r32(y, B2_GP_IO);
  }
  w16(y, B0_CTST, Y2_LED_STAT_ON);
  cfg_write(y, PCI_DEV_REG1, cfg_read(y, PCI_DEV_REG1) & ~PCI_Y2_PHY_DOWN);
  w8(y, B2_TST_CTRL1, TST_CFG_WRITE_OFF);
  cfg_read(y, PCI_DEV_REG1);
  w16(y, GMAC_LINK_CTRL, GMLC_RST_SET);
  w16(y, GMAC_LINK_CTRL, GMLC_RST_CLR);
  w32(y, B0_HWE_IMSK, 0);
  w8(y, B2_TI_CTRL, TIM_STOP);
  w8(y, B2_TI_CTRL, TIM_CLR_IRQ);
  w8(y, TXA_CTRL, 0x02);                /* transmit arbiter on */
  w16(y, B3_RI_CTRL, 0x0002);           /* RAM interface out of reset */
  for (i = 0; i < 12; i++)
    w8(y, B3_RI_WTO_R1 + (uint32_t)i, 36);

  /* status ring; timers in 125 MHz clock ticks */
  memset(y->st, 0, 4096);
  w32(y, STAT_CTRL, SC_STAT_RST_SET);
  w32(y, STAT_CTRL, SC_STAT_RST_CLR);
  w32(y, STAT_LIST_LO, y->st_phys);
  w32(y, STAT_LIST_HI, 0);
  w16(y, STAT_LAST_IDX, ST_LE - 1);
  w16(y, STAT_TX_IDX_TH, 10);
  w8(y, STAT_FIFO_WM, 16);
  w8(y, STAT_FIFO_ISR_WM, 16);
  w32(y, STAT_TX_TIMER_INI, 1000 * 125);
  w32(y, STAT_ISR_TIMER_INI, 20 * 125);
  w32(y, STAT_LEV_TIMER_INI, 100 * 125);
  w32(y, STAT_CTRL, SC_STAT_OP_ON);
  w8(y, STAT_TX_TIMER_CTRL, TIM_START);
  w8(y, STAT_ISR_TIMER_CTRL, TIM_START);
  w8(y, STAT_LEV_TIMER_CTRL, TIM_START);

  /* MAC and PHY */
  for (i = 0; i < 6; i++)
    y->nic.mac[i] = r8(y, B2_MAC_1 + (uint32_t)i);
  w8(y, GPHY_CTRL, GPC_RST_SET);
  w8(y, GPHY_CTRL, GPC_RST_CLR);
  w8(y, GMAC_CTRL, GMC_RST_CLR);
  w8(y, GMAC_CTRL, GMC_PAUSE_OFF);
  r8(y, GMAC_IRQ_SRC);
  w8(y, GMAC_IRQ_MSK, 0);
  /* PHY: advertise everything, restart auto-negotiation */
  phy_write(y, 4, 0x01e1);              /* 10/100, half and full duplex */
  phy_write(y, 9, 0x0300);              /* 1000 */
  phy_write(y, 0, 0x9200);              /* reset, auto-negotiate, restart */
  gm_write(y, GM_GP_CTRL, 0);           /* speed and duplex follow the PHY */
  gm_write(y, GM_TX_CTRL, 0x0f << 10);
  gm_write(y, GM_RX_CTRL, GM_RXCR_UCF_ENA | GM_RXCR_CRC_DIS);   /* own unicast, all multicast */
  gm_write(y, GM_TX_FLOW_CTRL, 0xffff);
  gm_write(y, GM_TX_PARAM, 3 << 14 | 0x0b << 9 | 0x1c << 4 | 4);
  gm_write(y, GM_SERIAL_MODE, 4 << 11 | 1 << 9 | 0x1e);
  for (i = 0; i < 3; i++)
  {
    mac16 = (uint16_t)(y->nic.mac[i * 2] | y->nic.mac[i * 2 + 1] << 8);
    gm_write(y, GM_SRC_ADDR_1L + (uint32_t)i * 4, mac16);
    gm_write(y, GM_SRC_ADDR_2L + (uint32_t)i * 4, mac16);
  }
  for (i = 0; i < 4; i++)
    gm_write(y, GM_MC_ADDR_H1 + (uint32_t)i * 4, 0xffff);
  gm_write(y, GM_TX_IRQ_MSK, 0);
  gm_write(y, GM_RX_IRQ_MSK, 0);
  gm_write(y, GM_TR_IRQ_MSK, 0);

  /* MAC FIFOs */
  w8(y, RX_GMF_CTRL_T, GMF_RST_CLR);
  w32(y, RX_GMF_CTRL_T, GMF_OPER_ON | GMF_RX_F_FL_ON);
  w16(y, RX_GMF_FL_MSK, GMR_FS_ANY_ERR);
  w16(y, RX_GMF_FL_THR, 0x0a + 1);
  w8(y, TX_GMF_CTRL_T, GMF_RST_CLR);
  w16(y, TX_GMF_CTRL_T, GMF_OPER_ON);
  ram = r8(y, B2_E_0) * 4u;
  if (!ram)
  {
    /* no RAM buffer: pause thresholds at the MAC, store and forward */
    w16(y, RX_GMF_UP_THR, 1020 / 8);
    w16(y, RX_GMF_LP_THR, 768 / 8);
    w32(y, TX_GMF_CTRL_T, TX_STFW_ENA);
  }
  else
  {
    uint32_t rxs = ram < 16 ? ram / 2 : 8 + (2 * (ram - 16)) / 3;
    ramset(y, Q_R1, 0, rxs);
    ramset(y, Q_XA1, rxs, ram - rxs);
    w8(y, RB_CTRL + Q_XS1, RB_RST_SET); /* no synchronous queue */
  }

  /* transmit queue */
  memset(y->tx, 0, 4096);
  qset(y, Q_XA1);
  if (y->chip == 0xb4 && y->rev == 0)   /* Yukon EC Ultra A0 */
  {
    w32(y, Q_TEST + Q_XA1, F_TX_CHK_AUTO_OFF);
    w16(y, Q_AL + Q_XA1, 0x01a0);
  }
  prefetch_init(y, Q_XA1, y->tx_phys, TX_LE - 1);

  /* receive queue with all buffers */
  memset(y->rx, 0, 4096);
  qset(y, Q_R1);
  w32(y, Q_WM + Q_R1, BMU_WM_PEX);
  if (y->chip == 0xb4 && y->rev > 0)
    w32(y, Q_TEST + Q_R1, F_M_RX_RAM_DIS);
  prefetch_init(y, Q_R1, y->rx_phys, RX_LE - 1);
  w32(y, Q_CSR + Q_R1, BMU_DIS_RX_CHKSUM);
  w32(y, Q_CSR + Q_R1, BMU_DIS_RX_RSS);
  for (i = 0; i < NRX; i++)
    rx_add(y, (uint32_t)i);
  w32(y, RX_GMF_CTRL_T, RX_TRUNC_OFF);
  w16(y, PREF_PUT_IDX + Q_R1, (uint16_t)y->rx_put);

  v = gm_read(y, GM_GP_CTRL);
  gm_write(y, GM_GP_CTRL, (uint16_t)(v | GM_GPCR_RX_ENA | GM_GPCR_TX_ENA));

  y->nic.name = "Marvell Yukon-2";
  y->nic.send = y_send;
  y->nic.recv = y_recv;
  y->nic.link = y_link;
  y->nic.debug = y_debug;
  have_card = 1;
  kprintf("yukon2: RAM buffer %u KB, PHY id %04x:%04x\n", ram, phy_read(y, 2), phy_read(y, 3));
  nic_register(&y->nic);
}

void yukon2_init(void)
{
  pci_scan(0x020000, found);
}
