/* MDVMM core - initialization and trap dispatch */
#include "core.h"

void trap_dispatch(struct frame *f);
void core_main(struct bootinfo *bi, uint32_t free_phys);

void trap_dispatch(struct frame *f)
{
  int user = !(f->eflags & EFL_VM) && (f->cs & 3) == 3;

  if (f->vector >= 0x20 && f->vector < 0x30)
  {
    dma_irq();                  /* bounced DMA data before any handler */
    if (user)
      dpmi_irq(f, (int)f->vector - 0x20);
    else
      v86_irq(f, (int)f->vector - 0x20);
    return;
  }
  if (f->eflags & EFL_VM)
  {
    v86_exception(f);
    return;
  }
  if (user)
  {
    dpmi_trap(f);
    return;
  }
  panic_frame(f, "exception in monitor");
}

void core_main(struct bootinfo *bi, uint32_t free_phys)
{
  /* the loader's copy lives in memory DOS will reuse */
  memcpy(&boot, bi, sizeof(boot));

  kprintf("\nMDVMM core: loaded at %08x, %u KB pool\n", boot.pool_phys,
          boot.pool_size / 1024);
  if (boot.magic != BOOTINFO_MAGIC || boot.version != BOOTINFO_VERSION)
    panic("bad boot information");

  mm_init(free_phys);
  cpu_init();
  dma_init();
  dpmi_init();
  v86_start();
}
