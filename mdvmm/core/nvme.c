/* MDVMM core - NVMe disk driver for the virtual BIOS
 *
 * Finds NVMe controllers on PCI (class 01:08:02), resets them, sets up
 * the admin queue and one I/O queue pair, and registers every active
 * namespace with 512-byte blocks as a BIOS hard disk (bios.c). Commands
 * are issued one at a time and their completion is polled, interrupts
 * stay off. Transfers use the caller's pool pages through PRP entries
 * (a PRP list for more than two pages). Not used on DOS boots.
 */
#include "core.h"

#define MAX_CTRL      4
#define MAX_NS        8
#define QSIZE         16                /* entries per queue */

/* controller registers */
#define R_CAP         0x00
#define R_VS          0x08
#define R_CC          0x14
#define R_CSTS        0x1c
#define R_AQA         0x24
#define R_ASQ         0x28
#define R_ACQ         0x30

#define CC_EN         0x00000001u
#define CC_IOSQES     (6u << 16)        /* 64 byte submission entries */
#define CC_IOCQES     (4u << 20)        /* 16 byte completion entries */
#define CSTS_RDY      0x1
#define CSTS_CFS      0x2

/* admin commands */
#define A_CREATE_SQ   0x01
#define A_CREATE_CQ   0x05
#define A_IDENTIFY    0x06
/* I/O commands */
#define IO_FLUSH      0x00
#define IO_WRITE      0x01
#define IO_READ       0x02

struct queue {
  uint32_t sq, cq;                      /* physical pages */
  uint16_t tail, head;
  uint8_t phase;
  int id;
};

struct ctrl {
  volatile uint8_t *regs;
  uint32_t dstrd;                       /* doorbell stride, bytes */
  uint32_t timeout_ms;
  struct queue admin, io;
  uint32_t prp_list;                    /* page for PRP lists */
  uint16_t cid;
  char model[41];
};

struct ns {
  struct disk disk;
  struct ctrl *c;
  uint32_t nsid;
};

static struct ctrl ctrls[MAX_CTRL];
static struct ns nss[MAX_NS];
static int nctrl, nns;

static uint32_t rd32(struct ctrl *c, int r)
{
  return *(volatile uint32_t *)(c->regs + r);
}

static void wr32(struct ctrl *c, int r, uint32_t v)
{
  *(volatile uint32_t *)(c->regs + r) = v;
}

static void wr64(struct ctrl *c, int r, uint32_t v)
{
  wr32(c, r, v);
  wr32(c, r + 4, 0);
}

static int wait_ready(struct ctrl *c, uint32_t want)
{
  uint32_t i;

  for (i = 0; i < c->timeout_ms * 1000; i++)
  {
    uint32_t s = rd32(c, R_CSTS);
    if (s & CSTS_CFS)
      return 0;
    if ((s & CSTS_RDY) == want)
      return 1;
    udelay(1);
  }
  return 0;
}

/* submit one command (cmd[16] dwords) on q and wait for its completion;
   returns the status field (0 = success), or -1 on timeout */
static int submit(struct ctrl *c, struct queue *q, uint32_t *cmd)
{
  uint32_t *sqe = (uint32_t *)pool_ptr(q->sq) + q->tail * 16;
  volatile uint32_t *cqe;
  uint16_t cid = ++c->cid;
  uint32_t i, status;

  cmd[0] = (cmd[0] & 0xffff) | (uint32_t)cid << 16;
  memcpy(sqe, cmd, 64);
  q->tail = (q->tail + 1) % QSIZE;
  wr32(c, 0x1000 + (2 * q->id) * c->dstrd, q->tail);

  cqe = (volatile uint32_t *)pool_ptr(q->cq) + q->head * 4;
  for (i = 0; i < c->timeout_ms * 1000; i++)
  {
    if (((cqe[3] >> 16) & 1) == q->phase)
      break;
    udelay(1);
  }
  if (((cqe[3] >> 16) & 1) != q->phase)
    return -1;
  status = cqe[3] >> 17;
  q->head = (q->head + 1) % QSIZE;
  if (!q->head)
    q->phase ^= 1;
  wr32(c, 0x1000 + (2 * q->id + 1) * c->dstrd, q->head);
  return (int)(status & 0x7fff);
}

static int queue_alloc(struct queue *q, int id)
{
  q->sq = page_alloc();
  q->cq = page_alloc();
  q->tail = q->head = 0;
  q->phase = 1;
  q->id = id;
  return q->sq && q->cq;
}

static int identify(struct ctrl *c, uint32_t cns, uint32_t nsid, uint32_t page)
{
  uint32_t cmd[16];

  memset(cmd, 0, sizeof(cmd));
  cmd[0] = A_IDENTIFY;
  cmd[1] = nsid;
  cmd[6] = page;
  cmd[10] = cns;
  return submit(c, &c->admin, cmd);
}

static int nvme_rw(struct disk *d, uint64_t lba, uint32_t n, const uint32_t *pages, int write)
{
  struct ns *s = d->drv;
  struct ctrl *c = s->c;
  uint32_t cmd[16], npages = (n * 512 + 4095) / 4096, i;
  int st;

  if (!n || n > DISK_MAX_SECTORS || lba + n > d->sectors)
    return 0x04;
  memset(cmd, 0, sizeof(cmd));
  cmd[0] = write ? IO_WRITE : IO_READ;
  cmd[1] = s->nsid;
  cmd[6] = pages[0];
  if (npages == 2)
    cmd[8] = pages[1];
  else if (npages > 2)
  {
    uint64_t *list = pool_ptr(c->prp_list);
    for (i = 1; i < npages; i++)
      list[i - 1] = pages[i];
    cmd[8] = c->prp_list;
  }
  cmd[10] = (uint32_t)lba;
  cmd[11] = (uint32_t)(lba >> 32);
  cmd[12] = n - 1;
  st = submit(c, &c->io, cmd);
  if (st)
  {
    kprintf("nvme: %s: %s at %u failed, status %x\n", d->model, write ? "write" : "read",
            (uint32_t)lba, (uint32_t)st);
    return st < 0 ? 0x80 : write ? 0x03 : 0x10;
  }
  return 0;
}

static int nvme_flush(struct disk *d)
{
  struct ns *s = d->drv;
  uint32_t cmd[16];

  memset(cmd, 0, sizeof(cmd));
  cmd[0] = IO_FLUSH;
  cmd[1] = s->nsid;
  return submit(s->c, &s->c->io, cmd) ? 0x80 : 0;
}

static void copy_string(char *dst, const uint8_t *src, int len)
{
  int i;

  memcpy(dst, src, len);
  for (i = len; i > 0 && (dst[i - 1] == ' ' || !dst[i - 1]); i--)
    ;
  dst[i] = 0;
}

static void ctrl_init(struct ctrl *c)
{
  uint32_t cap_lo = rd32(c, R_CAP), cap_hi = rd32(c, R_CAP + 4);
  uint32_t page, cmd[16], nn, i;
  const uint8_t *id;

  c->dstrd = 4u << (cap_hi & 0xf);
  if (0x1000 + 4 * c->dstrd > 0x2000)
  {
    kprintf("nvme: doorbell stride %u not supported\n", c->dstrd);
    return;
  }
  c->timeout_ms = ((cap_lo >> 24) & 0xff) * 500 + 500;
  if ((cap_hi >> 16) & 0xf)
  {
    kprintf("nvme: controller does not support 4K pages\n");
    return;
  }
  if ((cap_lo & 0xffff) + 1 < QSIZE)
  {
    kprintf("nvme: queues too small\n");
    return;
  }

  /* reset, admin queue, enable */
  wr32(c, R_CC, rd32(c, R_CC) & ~CC_EN);
  if (!wait_ready(c, 0))
  {
    kprintf("nvme: controller does not stop\n");
    return;
  }
  if (!queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1) || !(c->prp_list = page_alloc()) ||
      !(page = page_alloc()))
    return;
  wr32(c, R_AQA, (QSIZE - 1) << 16 | (QSIZE - 1));
  wr64(c, R_ASQ, c->admin.sq);
  wr64(c, R_ACQ, c->admin.cq);
  wr32(c, R_CC, CC_EN | CC_IOSQES | CC_IOCQES);
  if (!wait_ready(c, CSTS_RDY))
  {
    kprintf("nvme: controller does not start\n");
    return;
  }

  /* controller: model, number of namespaces */
  if (identify(c, 1, 0, page))
    return;
  id = pool_ptr(page);
  copy_string(c->model, id + 24, 40);
  nn = *(const uint32_t *)(id + 516);

  /* I/O queue pair 1, physically contiguous, no interrupts */
  memset(cmd, 0, sizeof(cmd));
  cmd[0] = A_CREATE_CQ;
  cmd[6] = c->io.cq;
  cmd[10] = (QSIZE - 1) << 16 | 1;
  cmd[11] = 1;
  if (submit(c, &c->admin, cmd))
    return;
  memset(cmd, 0, sizeof(cmd));
  cmd[0] = A_CREATE_SQ;
  cmd[6] = c->io.sq;
  cmd[10] = (QSIZE - 1) << 16 | 1;
  cmd[11] = 1u << 16 | 1;
  if (submit(c, &c->admin, cmd))
    return;
  kprintf("nvme: %s, version %x, %u namespace(s)\n", c->model, rd32(c, R_VS), nn);

  for (i = 1; i <= nn && i <= 16 && nns < MAX_NS; i++)
  {
    struct ns *s = &nss[nns];
    uint32_t lbads;
    uint64_t size;

    if (identify(c, 0, i, page))
      continue;
    size = *(const uint64_t *)id;
    lbads = id[128 + (id[26] & 0xf) * 4 + 2];
    if (!size)
      continue;                         /* inactive */
    if (lbads != 9)
    {
      kprintf("nvme: namespace %u: %u byte blocks, not supported\n", i, 1u << lbads);
      continue;
    }
    memset(s, 0, sizeof(*s));
    s->c = c;
    s->nsid = i;
    s->disk.sectors = size;
    s->disk.drv = s;
    s->disk.rw = nvme_rw;
    s->disk.flush = nvme_flush;
    memcpy(s->disk.model, c->model, sizeof(s->disk.model));
    kprintf("nvme: namespace %u: %u MB\n", i, (uint32_t)(size >> 11));
    nns++;
    disk_register(&s->disk);
  }
  page_free(page);
}

static void found(int bus, int dev, int fn)
{
  uint64_t bar = pci_bar(bus, dev, fn, 0x10);
  struct ctrl *c;

  if (nctrl == MAX_CTRL || !bar)
    return;
  c = &ctrls[nctrl++];
  memset(c, 0, sizeof(*c));
  pci_enable(bus, dev, fn);
  c->regs = mmio_map(bar, 0x2000);
  if (!c->regs)
  {
    kprintf("nvme: %02x:%02x.%x: cannot map registers at %x:%08x\n", bus, dev, fn,
            (uint32_t)(bar >> 32), (uint32_t)bar);
    return;
  }
  kprintf("nvme: controller %02x:%02x.%x, registers at %x:%08x\n", bus, dev, fn,
          (uint32_t)(bar >> 32), (uint32_t)bar);
  ctrl_init(c);
}

void nvme_init(void)
{
  pci_scan(0x010802, found);            /* mass storage, NVM, NVMe */
}
