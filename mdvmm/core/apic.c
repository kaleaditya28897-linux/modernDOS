/* MDVMM core - interrupt routing after a UEFI boot
 *
 * DOS wants the PC/AT arrangement: the 8254 timer on IRQ0 of the 8259
 * PICs, their INTR line going to the CPU. UEFI firmware does not have to
 * leave the machine that way:
 *  - the local APIC must pass the PIC's INTR through (LINT0 = ExtINT,
 *    "virtual wire" mode) and should not raise interrupts of its own
 *  - an I/O APIC with enabled entries would deliver the same IRQs a
 *    second time, on vectors nobody handles
 *  - an HPET in "legacy replacement" mode takes IRQ0 away from the 8254
 * apic_setup() puts all that right and logs what it found; timer_selftest()
 * then checks that a timer tick really arrives, so a machine where it does
 * not shows a message instead of a silent hang.
 */
#include "core.h"

#define LAPIC_ID      0x020
#define LAPIC_VER     0x030
#define LAPIC_TPR     0x080
#define LAPIC_SVR     0x0f0
#define LAPIC_LVT_CMCI  0x2f0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_THERM 0x330
#define LAPIC_LVT_PERF  0x340
#define LAPIC_LINT0   0x350
#define LAPIC_LINT1   0x360
#define LAPIC_LVT_ERR 0x370
#define LVT_MASKED    0x10000u

static volatile uint32_t *lapic;

static uint64_t rdmsr(uint32_t msr)
{
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return (uint64_t)hi << 32 | lo;
}

static uint32_t lr(uint32_t reg)
{
  return lapic[reg / 4];
}

static void lw(uint32_t reg, uint32_t v)
{
  lapic[reg / 4] = v;
}

void apic_setup(void)
{
  uint32_t a, b, c, d;
  uint64_t base;
  volatile uint32_t *io, *hpet;

  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  (void)a; (void)b; (void)c;
  if (!(d & (1u << 9)))
    kprintf("apic: no local APIC\n");
  else
  {
    base = rdmsr(0x1b);
    if (!(base & 0x800))
      kprintf("apic: local APIC disabled (base %08x): the PIC drives the CPU directly\n",
              (uint32_t)base);
    else if (base & 0x400)
      kprintf("apic: x2APIC mode, left alone\n");
    else if ((lapic = mmio_map(base & 0xfffff000ull, 0x1000)))
    {
      uint32_t maxlvt = (lr(LAPIC_VER) >> 16) & 0xff;
      kprintf("apic: local APIC at %08x: SVR %x LINT0 %x LINT1 %x timer %x TPR %x\n",
              (uint32_t)(base & 0xfffff000u), lr(LAPIC_SVR), lr(LAPIC_LINT0), lr(LAPIC_LINT1),
              lr(LAPIC_LVT_TIMER), lr(LAPIC_TPR));
      lw(LAPIC_LVT_TIMER, LVT_MASKED);
      lw(LAPIC_LVT_ERR, LVT_MASKED);
      if (maxlvt >= 4)
        lw(LAPIC_LVT_PERF, LVT_MASKED);
      if (maxlvt >= 5)
        lw(LAPIC_LVT_THERM, LVT_MASKED);
      if (maxlvt >= 6)
        lw(LAPIC_LVT_CMCI, LVT_MASKED);
      lw(LAPIC_TPR, 0);
      lw(LAPIC_LINT0, 0x700);           /* ExtINT: the 8259's INTR */
      lw(LAPIC_LINT1, 0x400);           /* NMI */
      lw(LAPIC_SVR, 0x1ff);             /* enabled, spurious vector FFh */
      kprintf("apic: virtual wire mode set (LINT0 = ExtINT)\n");
    }
  }

  /* I/O APIC (FEC00000h on PC chipsets): mask every entry */
  io = mmio_map(0xfec00000ull, 0x1000);
  if (io)
  {
    uint32_t ver, n, i, live = 0;
    io[0] = 1;
    ver = io[4];
    n = ((ver >> 16) & 0xff) + 1;
    if (ver != 0xffffffffu && ver != 0 && n <= 240)
    {
      for (i = 0; i < n; i++)
      {
        uint32_t lo;
        io[0] = 0x10 + i * 2;
        lo = io[4];
        if (!(lo & LVT_MASKED))
        {
          live++;
          io[0] = 0x10 + i * 2;
          io[4] = lo | LVT_MASKED;
        }
      }
      kprintf("apic: I/O APIC with %u entries, %u were enabled (now masked)\n", n, live);
    }
    else
      kprintf("apic: no I/O APIC at FEC00000\n");
  }

  /* HPET (FED00000h): the 8254 must own IRQ0 */
  hpet = mmio_map(0xfed00000ull, 0x1000);
  if (hpet)
  {
    uint32_t cap = hpet[0], cfg;
    if (cap != 0xffffffffu && cap != 0 && (cap >> 16) != 0xffff)
    {
      cfg = hpet[0x10 / 4];
      if (cfg & 2)
      {
        hpet[0x10 / 4] = cfg & ~2u;
        kprintf("hpet: legacy replacement mode was on (config %x): turned off\n", cfg);
      }
      else
        kprintf("hpet: present, config %x (the 8254 has IRQ0)\n", cfg);
    }
    else
      kprintf("hpet: none at FED00000\n");
  }
}

/* IRQ0 pending at the master PIC? Acknowledges it there (poll mode). */
static int pic_poll_irq0(void)
{
  uint8_t v;

  outb(0x20, 0x0c);                     /* OCW3: poll */
  v = inb(0x20);
  if (!(v & 0x80))
    return 0;
  outb(0x20, 0x60 | (v & 7));           /* specific EOI */
  return (v & 7) == 0;
}

/* wait `ticks` timer ticks (55 ms each) with interrupts off, by polling
   the PIC; returns 0 if the timer does not tick */
int pit_wait(uint32_t ticks)
{
  uint8_t mask = inb(0x21);
  uint32_t i, spin;

  outb(0x21, 0xfe);                     /* only IRQ0 */
  for (i = 0; i < ticks; i++)
  {
    for (spin = 0; spin < 3000000 && !pic_poll_irq0(); spin++)
      ;
    if (spin == 3000000)
    {
      outb(0x21, mask);
      return 0;
    }
  }
  outb(0x21, mask);
  return 1;
}

/* does a timer tick reach the PIC, and then the CPU? */
void timer_selftest(void)
{
  uint8_t mask = inb(0x21);
  uint32_t spin;
  int pic, cpu = 0;

  pic = pit_wait(2);
  if (!pic)
  {
    kprintf("timer: FAILED - the 8254 raises no IRQ0 at the PIC (IRR %02x, mask %02x)\n",
            (outb(0x20, 0x0a), inb(0x20)), mask);
    return;
  }
  /* now with interrupts on: the handler notes IRQ0 as pending */
  vm1.pending &= ~1u;
  outb(0x21, 0xfe);
  __asm__ volatile("sti");
  for (spin = 0; spin < 3000000 && !(vm1.pending & 1); spin++)
    inb(0x21);
  __asm__ volatile("cli");
  cpu = (vm1.pending & 1) != 0;
  if (cpu)
  {
    outb(0x20, 0x60);                   /* end of that interrupt */
    vm1.pending &= ~1u;
  }
  outb(0x21, mask);
  if (cpu)
    kprintf("timer: ok - the 8254 ticks and its interrupt reaches the CPU\n");
  else
  {
    kprintf("timer: FAILED - IRQ0 is raised at the PIC but no interrupt reaches the CPU\n");
    if (lapic)
      kprintf("timer:   local APIC SVR %x LINT0 %x TPR %x ISR0 %x IRR0 %x\n", lr(LAPIC_SVR),
              lr(LAPIC_LINT0), lr(LAPIC_TPR), lr(0x100), lr(0x200));
  }
}
