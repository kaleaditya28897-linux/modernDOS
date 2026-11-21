/* MDVMM core - physical memory and page mapping
 *
 * The loader hands over one contiguous physical pool. entry.asm maps it
 * at KBASE and identity maps the first 4MB (V86 view of the machine).
 * Page tables are reachable through the recursive mapping at 0xFFC00000.
 */
#include "core.h"

#define PT_BASE   0xFFC00000u           /* recursive: all page tables */
#define PD_BASE   0xFFFFF000u           /* recursive: page directory  */
#define KMAP_BASE 0xEFC00000u           /* temporary physical windows */
#define KMAP_SLOTS 4

struct bootinfo boot;
static uint32_t next_free, pool_end;

static inline uint32_t *pte(uint32_t lin) { return (uint32_t *)(PT_BASE + (lin >> 12) * 4); }
static inline uint32_t *pde(uint32_t lin) { return (uint32_t *)(PD_BASE + (lin >> 22) * 4); }

static void *pool_virt(uint32_t phys) { return (void *)(KBASE + (phys - boot.pool_phys)); }

/* allocate one zeroed physical page from the pool (no freeing yet) */
uint32_t page_alloc(void)
{
  uint32_t p;

  if (next_free >= pool_end)
    panic("out of memory in pool");
  p = next_free;
  next_free += 4096;
  memset(pool_virt(p), 0, 4096);
  return p;
}

void mm_init(uint32_t free_phys)
{
  uint32_t pt;

  next_free = free_phys;
  pool_end = boot.pool_phys + boot.pool_size;

  /* page table for the kmap window */
  pt = page_alloc();
  *pde(KMAP_BASE) = pt | 3;
  invlpg((uint32_t)pte(KMAP_BASE));
  memset((void *)((uint32_t)pte(KMAP_BASE) & ~0xfffu), 0, 4096);

  kprintf("mm: pool %08x-%08x, %u KB free\n", boot.pool_phys, pool_end,
          (pool_end - next_free) / 1024);
}

/* map a physical page into a temporary window slot */
void *kmap(int slot, uint32_t phys)
{
  uint32_t lin = KMAP_BASE + slot * 4096;

  *pte(lin) = (phys & ~0xfffu) | 3;
  invlpg(lin);
  return (void *)(lin + (phys & 0xfff));
}

/* copy between arbitrary physical addresses */
void phys_copy(uint32_t dst, uint32_t src, uint32_t len)
{
  while (len)
  {
    uint32_t n = 4096 - (src & 0xfff);
    if (n > 4096 - (dst & 0xfff))
      n = 4096 - (dst & 0xfff);
    if (n > len)
      n = len;
    memcpy(kmap(1, dst), kmap(0, src), n);
    src += n;
    dst += n;
    len -= n;
  }
}
