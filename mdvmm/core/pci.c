/* MDVMM core - PCI configuration space (mechanism #1) and device MMIO */
#include "core.h"

static uint32_t mmio_next = MMIO_VIRT;

static uint32_t cfg_addr(int bus, int dev, int fn, int reg)
{
  return 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (reg & 0xfc);
}

uint32_t pci_read32(int bus, int dev, int fn, int reg)
{
  outl(0xcf8, cfg_addr(bus, dev, fn, reg));
  return inl(0xcfc);
}

void pci_write32(int bus, int dev, int fn, int reg, uint32_t v)
{
  outl(0xcf8, cfg_addr(bus, dev, fn, reg));
  outl(0xcfc, v);
}

/* call found() for every function with class:subclass:progif == cls
   (24 bits); the firmware has numbered all buses */
void pci_scan(uint32_t cls, void (*found)(int bus, int dev, int fn))
{
  int bus, dev, fn;

  for (bus = 0; bus < 256; bus++)
    for (dev = 0; dev < 32; dev++)
      for (fn = 0; fn < 8; fn++)
      {
        uint32_t id = pci_read32(bus, dev, fn, 0x00);
        if ((id & 0xffff) == 0xffff)
        {
          if (fn == 0)
            break;                      /* no device */
          continue;
        }
        if (pci_read32(bus, dev, fn, 0x08) >> 8 == cls)
          found(bus, dev, fn);
        if (fn == 0 && !(pci_read32(bus, dev, fn, 0x0c) & 0x00800000))
          break;                        /* single function device */
      }
}

/* memory space + bus master on, legacy INTx off (the drivers poll) */
void pci_enable(int bus, int dev, int fn)
{
  uint32_t cmd = pci_read32(bus, dev, fn, 0x04);
  pci_write32(bus, dev, fn, 0x04, (cmd & 0xffff) | 0x0006 | 0x0400);
}

/* memory BAR at reg (64-bit BARs use the next register too) */
uint64_t pci_bar(int bus, int dev, int fn, int reg)
{
  uint32_t lo = pci_read32(bus, dev, fn, reg);

  if (lo & 1)
    return 0;                           /* I/O space */
  if ((lo & 6) == 4)
    return (lo & ~0xfull) | (uint64_t)pci_read32(bus, dev, fn, reg + 4) << 32;
  return lo & ~0xfull;
}

/* map device registers (uncached) into the monitor; NULL if no room.
   Registers above 4GB (64-bit BARs) get 4MB PSE-36 pages. */
void *mmio_map(uint64_t phys, uint32_t size)
{
  uint32_t off = phys & 0xfff, lin = mmio_next, p;

  if (phys + size > 0x100000000ull)
  {
    uint64_t base = phys & ~0x3fffffull;
    uint32_t span = (uint32_t)(phys + size - base);
    lin = (mmio_next + 0x3fffff) & ~0x3fffffu;
    for (p = 0; p < span; p += 0x400000)
      if (lin + p + 0x400000 > MMIO_VIRT + MMIO_MAX || !map_large_mmio(lin + p, base + p))
        return 0;
    mmio_next = lin + ((span + 0x3fffff) & ~0x3fffffu);
    return (void *)(lin + (uint32_t)(phys - base));
  }

  size = (off + size + 4095) & ~4095u;
  if (mmio_next + size > MMIO_VIRT + MMIO_MAX)
    return 0;
  for (p = 0; p < size; p += 4096)
    if (!map_page(lin + p, (phys & ~0xfffu) + p, 0x1b))       /* PCD PWT */
      return 0;
  mmio_next += size;
  return (void *)(lin + off);
}

/* about one microsecond (a port 80h write takes ~1us on PCs) */
void udelay(uint32_t us)
{
  while (us--)
    outb(0x80, 0);
}
