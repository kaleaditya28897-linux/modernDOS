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
static uint32_t free_list;              /* freed pages, linked through page */
static uint32_t free_count;             /* pages on free_list */

static inline uint32_t *pte(uint32_t lin) { return (uint32_t *)(PT_BASE + (lin >> 12) * 4); }
static inline uint32_t *pde(uint32_t lin) { return (uint32_t *)(PD_BASE + (lin >> 22) * 4); }

static void *pool_virt(uint32_t phys) { return (void *)(KBASE + (phys - boot.pool_phys)); }

/* allocate one zeroed physical page from the pool (no freeing yet) */
uint32_t page_alloc(void)
{
  uint32_t p;

  if (free_list)
  {
    p = free_list;
    free_list = *(uint32_t *)pool_virt(p);
    free_count--;
  }
  else
  {
    if (next_free >= pool_end)
      return 0;
    p = next_free;
    next_free += 4096;
  }
  memset(pool_virt(p), 0, 4096);
  return p;
}

void page_free(uint32_t phys)
{
  *(uint32_t *)pool_virt(phys) = free_list;
  free_list = phys;
  free_count++;
}

uint32_t pages_free(void)
{
  return free_count + (pool_end - next_free) / 4096;
}

uint32_t pages_total(void)
{
  return boot.pool_size / 4096;
}

/* map one page at linear address lin; flags: 7 = user rw, 3 = supervisor.
   Page tables for user regions are created on demand. 0 on failure. */
int map_page(uint32_t lin, uint32_t phys, uint32_t flags)
{
  if (!(*pde(lin) & 1))
  {
    uint32_t pt = page_alloc();
    if (!pt)
      return 0;
    *pde(lin) = pt | 7;
    invlpg((uint32_t)pte(lin));
    memset((void *)((uint32_t)pte(lin) & ~0xfffu), 0, 4096);
  }
  *pte(lin) = (phys & ~0xfffu) | flags;
  invlpg(lin);
  return 1;
}

/* unmap a page, returns its physical address (0 if not mapped) */
uint32_t unmap_page(uint32_t lin)
{
  uint32_t e;

  if (!(*pde(lin) & 1))
    return 0;
  e = *pte(lin);
  *pte(lin) = 0;
  invlpg(lin);
  return (e & 1) ? (e & ~0xfffu) : 0;
}

uint32_t lin_to_pte(uint32_t lin)
{
  if (!(*pde(lin) & 1))
    return 0;
  return *pte(lin);
}

void mm_init(uint32_t free_phys)
{
  uint32_t pt;

  next_free = free_phys;
  pool_end = boot.pool_phys + boot.pool_size;

  /* page table for the kmap window */
  pt = page_alloc();
  if (!pt)
    panic("no memory for page tables");
  *pde(KMAP_BASE) = pt | 3;
  invlpg((uint32_t)pte(KMAP_BASE));
  memset((void *)((uint32_t)pte(KMAP_BASE) & ~0xfffu), 0, 4096);

  kprintf("mm: pool %08x-%08x, %u KB free\n", boot.pool_phys, pool_end,
          (pool_end - next_free) / 1024);
}

void *pool_ptr(uint32_t phys)
{
  return pool_virt(phys);
}

/* physically contiguous, size aligned block below 16MB (ISA DMA), taken
   from the untouched part of the pool; call early. 0 if not possible. */
uint32_t page_alloc_isa(uint32_t size)
{
  uint32_t p = (next_free + size - 1) & ~(size - 1);

  if (p + size > pool_end || p + size > 0x1000000)
    return 0;
  while (next_free < p)                 /* skipped pages stay usable */
  {
    page_free(next_free);
    next_free += 4096;
  }
  next_free = p + size;
  return p;
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
