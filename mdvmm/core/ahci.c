/* MDVMM core - AHCI (SATA) disk driver for the virtual BIOS
 *
 * Finds AHCI controllers on PCI, takes over every port with a SATA disk
 * and registers it as a BIOS hard disk (INT 13h, bios.c). Commands are
 * issued one at a time and polled with interrupts off: DOS does not
 * overlap disk I/O anyway. Data goes through the caller's pool pages
 * (one PRD entry per 4K page), so no physically contiguous buffers are
 * needed. Not used on DOS boots, where the PC BIOS owns the disks.
 */
#include "core.h"

#define MAX_DISKS    8

/* HBA registers */
#define HBA_CAP      0x00
#define HBA_GHC      0x04
#define HBA_PI       0x0c
#define HBA_VS       0x10
#define GHC_AE       0x80000000u

/* port registers, at 100h + port * 80h */
#define P_CLB        0x00
#define P_CLBU       0x04
#define P_FB         0x08
#define P_FBU        0x0c
#define P_IS         0x10
#define P_IE         0x14
#define P_CMD        0x18
#define P_TFD        0x20
#define P_SIG        0x24
#define P_SSTS       0x28
#define P_SERR       0x30
#define P_CI         0x38

#define CMD_ST       0x0001
#define CMD_SUD      0x0002
#define CMD_POD      0x0004
#define CMD_FRE      0x0010
#define CMD_FR       0x4000
#define CMD_CR       0x8000
#define IS_TFES      0x40000000u
#define TFD_ERR      0x01
#define TFD_DRQ      0x08
#define TFD_BSY      0x80

#define SIG_ATA      0x00000101u

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_READ_DMA       0xc8
#define ATA_WRITE_DMA      0xca
#define ATA_IDENTIFY       0xec
#define ATA_FLUSH_EXT      0xea

struct port {
  struct disk disk;
  volatile uint8_t *regs;
  uint32_t clb, fb, ctba;               /* physical: command list, FIS, table */
  int lba48;
};

static struct port ports[MAX_DISKS];
static int nports;

static uint32_t rd(struct port *p, int r)
{
  return *(volatile uint32_t *)(p->regs + r);
}

static void wr(struct port *p, int r, uint32_t v)
{
  *(volatile uint32_t *)(p->regs + r) = v;
}

/* wait until (reg & mask) == val, up to ms milliseconds */
static int wait_reg(struct port *p, int r, uint32_t mask, uint32_t val, uint32_t ms)
{
  uint32_t i;

  for (i = 0; i < ms * 1000; i++)
  {
    if ((rd(p, r) & mask) == val)
      return 1;
    udelay(1);
  }
  return 0;
}

/* one command in slot 0; pages: data pages (4K each), bytes total */
static int command(struct port *p, uint8_t cmd, uint64_t lba, uint32_t count,
                   const uint32_t *pages, uint32_t bytes, int write)
{
  uint32_t *hdr = pool_ptr(p->clb);
  uint8_t *tbl = pool_ptr(p->ctba);
  uint8_t *fis = tbl;
  uint32_t *prd = (uint32_t *)(tbl + 0x80);
  uint32_t i, nprd = (bytes + 4095) / 4096;

  memset(tbl, 0, 0x80 + nprd * 16);
  fis[0] = 0x27;                        /* register FIS, host to device */
  fis[1] = 0x80;                        /* command */
  fis[2] = cmd;
  fis[4] = (uint8_t)lba;
  fis[5] = (uint8_t)(lba >> 8);
  fis[6] = (uint8_t)(lba >> 16);
  fis[7] = 0x40;                        /* LBA mode */
  fis[8] = (uint8_t)(lba >> 24);
  fis[9] = (uint8_t)(lba >> 32);
  fis[10] = (uint8_t)(lba >> 40);
  fis[12] = (uint8_t)count;
  fis[13] = (uint8_t)(count >> 8);
  if (!p->lba48 && cmd != ATA_IDENTIFY)
    fis[7] |= (uint8_t)((lba >> 24) & 0x0f);
  for (i = 0; i < nprd; i++)
  {
    uint32_t n = bytes - i * 4096 < 4096 ? bytes - i * 4096 : 4096;
    prd[i * 4] = pages[i];
    prd[i * 4 + 1] = 0;
    prd[i * 4 + 3] = n - 1;
  }
  hdr[0] = 5 | (write ? 0x40 : 0) | nprd << 16;   /* FIS 5 dwords */
  hdr[1] = 0;
  hdr[2] = p->ctba;
  hdr[3] = 0;

  if (!wait_reg(p, P_TFD, TFD_BSY | TFD_DRQ, 0, 1000))
    return 0x80;                        /* timeout */
  wr(p, P_IS, 0xffffffffu);
  wr(p, P_CI, 1);
  for (i = 0; i < 10000000; i++)        /* about 10 s */
  {
    if (!(rd(p, P_CI) & 1))
      break;
    if (rd(p, P_IS) & IS_TFES)
      break;
    udelay(1);
  }
  if ((rd(p, P_IS) & IS_TFES) || (rd(p, P_TFD) & TFD_ERR))
  {
    kprintf("ahci: %s: command %02x failed, TFD %02x\n", p->disk.model, cmd, rd(p, P_TFD) & 0xff);
    wr(p, P_CMD, rd(p, P_CMD) & ~CMD_ST);      /* recover the port */
    wait_reg(p, P_CMD, CMD_CR, 0, 500);
    wr(p, P_SERR, 0xffffffffu);
    wr(p, P_IS, 0xffffffffu);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
    return write ? 0x03 : 0x10;         /* write protect / bad CRC-ECC */
  }
  if (rd(p, P_CI) & 1)
    return 0x80;
  return 0;
}

static int ahci_rw(struct disk *d, uint64_t lba, uint32_t n, const uint32_t *pages, int write)
{
  struct port *p = d->drv;
  uint8_t cmd;

  if (!n || n > DISK_MAX_SECTORS || lba + n > d->sectors)
    return 0x04;
  if (p->lba48)
    cmd = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
  else
    cmd = write ? ATA_WRITE_DMA : ATA_READ_DMA;
  return command(p, cmd, lba, n, pages, n * 512, write);
}

static int ahci_flush(struct disk *d)
{
  struct port *p = d->drv;
  return command(p, p->lba48 ? ATA_FLUSH_EXT : 0xe7, 0, 0, 0, 0, 0);
}

static void copy_model(char *dst, const uint16_t *id)
{
  int i;

  for (i = 0; i < 20; i++)
  {
    dst[i * 2] = (char)(id[27 + i] >> 8);
    dst[i * 2 + 1] = (char)id[27 + i];
  }
  for (i = 40; i > 0 && (dst[i - 1] == ' ' || !dst[i - 1]); i--)
    ;
  dst[i] = 0;
}

static void port_init(volatile uint8_t *hba, int n)
{
  struct port *p = &ports[nports];
  uint32_t id_page, ssts;
  const uint16_t *id;

  memset(p, 0, sizeof(*p));
  p->regs = hba + 0x100 + n * 0x80;
  ssts = rd(p, P_SSTS);
  if ((ssts & 0x0f) != 3 || rd(p, P_SIG) != SIG_ATA)
    return;                             /* no device, or ATAPI etc. */

  /* stop the port, give it our command list and FIS area */
  wr(p, P_CMD, rd(p, P_CMD) & ~CMD_ST);
  if (!wait_reg(p, P_CMD, CMD_CR, 0, 500))
    return;
  wr(p, P_CMD, rd(p, P_CMD) & ~CMD_FRE);
  if (!wait_reg(p, P_CMD, CMD_FR, 0, 500))
    return;
  p->clb = page_alloc();                /* 1K list + 256 byte FIS */
  p->ctba = page_alloc();
  id_page = page_alloc();
  if (!p->clb || !p->ctba || !id_page)
    return;
  p->fb = p->clb + 0x400;
  wr(p, P_CLB, p->clb);
  wr(p, P_CLBU, 0);
  wr(p, P_FB, p->fb);
  wr(p, P_FBU, 0);
  wr(p, P_IE, 0);
  wr(p, P_SERR, 0xffffffffu);
  wr(p, P_IS, 0xffffffffu);
  wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE | CMD_SUD | CMD_POD);
  if (!wait_reg(p, P_TFD, TFD_BSY | TFD_DRQ, 0, 2000))
    return;
  wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);

  p->disk.drv = p;
  p->disk.rw = ahci_rw;
  p->disk.flush = ahci_flush;
  memcpy(p->disk.model, "SATA disk", 10);
  if (command(p, ATA_IDENTIFY, 0, 0, &id_page, 512, 0))
    return;
  id = pool_ptr(id_page);
  copy_model(p->disk.model, id);
  p->lba48 = (id[83] & 0x0400) != 0;
  if (p->lba48)
    p->disk.sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 |
                      (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
  else
    p->disk.sectors = (uint32_t)id[60] | (uint32_t)id[61] << 16;
  page_free(id_page);
  if (!p->disk.sectors)
    return;
  kprintf("ahci: port %d: %s, %u MB%s\n", n, p->disk.model,
          (uint32_t)(p->disk.sectors >> 11), p->lba48 ? ", LBA48" : "");
  nports++;
  disk_register(&p->disk);
}

static void found(int bus, int dev, int fn)
{
  uint64_t abar = pci_bar(bus, dev, fn, 0x24);
  volatile uint8_t *hba;
  uint32_t pi;
  int n;

  if (!abar)
    return;
  pci_enable(bus, dev, fn);
  hba = mmio_map(abar, 0x1100);
  if (!hba)
    return;
  *(volatile uint32_t *)(hba + HBA_GHC) |= GHC_AE;
  *(volatile uint32_t *)(hba + HBA_GHC) &= ~2u;   /* no interrupts */
  pi = *(volatile uint32_t *)(hba + HBA_PI);
  kprintf("ahci: controller %02x:%02x.%x, version %x, ports %08x\n", bus, dev, fn,
          *(volatile uint32_t *)(hba + HBA_VS), pi);
  for (n = 0; n < 32 && nports < MAX_DISKS; n++)
    if (pi & (1u << n))
      port_init(hba, n);
}

void ahci_init(void)
{
  pci_scan(0x010601, found);            /* mass storage, SATA, AHCI 1.0 */
}
