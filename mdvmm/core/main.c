/* MDVMM core - initialization and trap dispatch */
#include "core.h"

void trap_dispatch(struct frame *f);
void core_main(struct bootinfo *bi, uint32_t free_phys);

void trap_dispatch(struct frame *f)
{
  int user = !(f->eflags & EFL_VM) && (f->cs & 3) == 3;

  if (f->vector == 0xff)
    return;                     /* local APIC spurious interrupt: no EOI */
  if (f->vector == 2)
  {
    /* NMI: on PCs a PCI system error (SERR#) or an I/O channel check.
       Note it and go on: stopping would hide what the machine was doing. */
    static int nmis;
    uint8_t why = inb(0x61);
    if (nmis++ < 5)
      kprintf("NMI: port 61h = %02x (%s) at %04x:%08x\n", why,
              why & 0x80 ? "PCI SERR#" : why & 0x40 ? "I/O check" : "unknown source",
              f->cs, f->eip);
    outb(0x61, (why & 0x0f) | 0x0c);    /* clear and disable both sources */
    return;
  }
  if (f->vector >= 0x20 && f->vector < 0x30)
  {
    dma_irq();                  /* bounced DMA data before any handler */
    if (user)
      dpmi_hwirq(f, (int)f->vector - 0x20);
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
    if (f->vector == 1 && dpmi_vga_debug(f))
      return;                   /* an I/O breakpoint of the monitor's VGA */
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
  if (boot.boot_type == BOOT_UEFI)
    fbcon_init();               /* the log on the screen from here on */
  dma_init();
  dpmi_init();
  if (boot.boot_type == BOOT_UEFI)
    bios_init();                /* no PC BIOS: provide one, boot DOS; it
                                   starts the network cards (after the timer) */
  else
    nic_init();
  v86_start();
}
