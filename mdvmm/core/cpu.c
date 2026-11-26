/* MDVMM core - descriptor tables, TSS with I/O permission bitmap, PIC */
#include "core.h"

struct tss {
  uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2;
  uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t es, cs, ss, ds, fs, gs, ldt;
  uint16_t trap, iomap_base;
  uint8_t iomap[8192];
  uint8_t iomap_end;            /* must be 0xFF */
} __attribute__((packed));

struct gdt_entry { uint16_t limit_lo, base_lo; uint8_t base_mid, access, gran, base_hi; } __attribute__((packed));
struct idt_entry { uint16_t off_lo, sel; uint8_t zero, type; uint16_t off_hi; } __attribute__((packed));
struct dtr { uint16_t limit; uint32_t base; } __attribute__((packed));

static struct tss tss __attribute__((aligned(16)));
struct gdt_entry gdt_table[9] __attribute__((aligned(16)));
#define gdt gdt_table
static struct idt_entry idt[256] __attribute__((aligned(16)));

void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
  gdt[i].limit_lo = limit & 0xffff;
  gdt[i].base_lo = base & 0xffff;
  gdt[i].base_mid = (base >> 16) & 0xff;
  gdt[i].access = access;
  gdt[i].gran = (gran & 0xf0) | ((limit >> 16) & 0x0f);
  gdt[i].base_hi = base >> 24;
}

/* load an LDT of `entries` descriptors at linear address base */
void ldt_load(void *base, uint32_t entries)
{
  gdt_set(LDT_SEL >> 3, (uint32_t)base, entries * 8 - 1, 0x82, 0x00);
  __asm__ volatile("lldt %w0" :: "r"(LDT_SEL));
}

/* raw descriptor from the GDT, for selector base lookups */
const void *gdt_entry(uint16_t sel)
{
  return &gdt[sel >> 3];
}

void io_trap(uint16_t port, int trap)
{
  if (trap)
    tss.iomap[port >> 3] |= 1 << (port & 7);
  else
    tss.iomap[port >> 3] &= ~(1 << (port & 7));
}

/* move the PIC vectors away from the CPU exceptions: IRQ0-15 -> 20h-2Fh */
static void pic_remap(void)
{
  uint8_t m1 = inb(0x21), m2 = inb(0xa1);

  outb(0x20, 0x11);
  outb(0xa0, 0x11);
  outb(0x21, 0x20);
  outb(0xa1, 0x28);
  outb(0x21, 0x04);
  outb(0xa1, 0x02);
  outb(0x21, 0x01);
  outb(0xa1, 0x01);
  outb(0x21, m1);
  outb(0xa1, m2);
}

void cpu_init(void)
{
  struct dtr d;
  int i;

  gdt_set(0, 0, 0, 0, 0);
  gdt_set(1, 0, 0xfffff, 0x9a, 0xc0);          /* KCODE */
  gdt_set(2, 0, 0xfffff, 0x92, 0xc0);          /* KDATA */
  gdt_set(3, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);
  gdt_set(7, 0, 0xfffff, 0x92, 0xc0);          /* ESPFIX_SEL, base set on use */
  d.limit = sizeof(gdt) - 1;
  d.base = (uint32_t)gdt;
  gdt_load(&d);

  memset(&tss, 0, sizeof(tss));
  tss.ss0 = KDATA;
  tss.esp0 = (uint32_t)kstack_top;
  tss.iomap_base = offsetof(struct tss, iomap);
  tss.iomap_end = 0xff;
  __asm__ volatile("ltr %w0" :: "r"(TSS_SEL));

  /* all gates DPL 0: INT n, INT3, INTO from V86 raise #GP and are emulated */
  for (i = 0; i < 256; i++)
  {
    idt[i].off_lo = isr_table[i] & 0xffff;
    idt[i].off_hi = isr_table[i] >> 16;
    idt[i].sel = KCODE;
    idt[i].zero = 0;
    idt[i].type = 0x8e;
  }
  d.limit = sizeof(idt) - 1;
  d.base = (uint32_t)idt;
  __asm__ volatile("lidt %0" :: "m"(d));

  pic_remap();

  /* A20 control (keyboard controller, port 92h) must stay with us */
  io_trap(0x60, 1);
  io_trap(0x64, 1);
  io_trap(0x92, 1);
}
