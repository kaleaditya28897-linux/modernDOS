/* MDVMM core - virtual 8086 monitor
 *
 * DOS runs in V86 mode with IOPL 0, so CLI/STI/PUSHF/POPF/INT/IRET/HLT
 * raise #GP and are emulated here against a virtual interrupt flag. The
 * real IF stays set while in V86 so hardware interrupts always reach the
 * monitor; they are reflected into the DOS interrupt vector table when
 * the virtual IF allows it, otherwise kept pending.
 */
#include "core.h"

struct vm vm1;

/* ---------------------------------------------------------------- */
/* V86 memory and stack                                             */

static uint8_t fetch8(struct frame *f, uint32_t off)
{
  return *(uint8_t *)rm_ptr(f->cs, off);
}

void v86_push16(struct frame *f, uint16_t v)
{
  uint16_t sp = (uint16_t)(f->esp - 2);
  *(uint16_t *)rm_ptr(f->ss, sp) = v;
  f->esp = (f->esp & 0xffff0000u) | sp;
}

static void push32(struct frame *f, uint32_t v)
{
  uint16_t sp = (uint16_t)(f->esp - 4);
  *(uint32_t *)rm_ptr(f->ss, sp) = v;
  f->esp = (f->esp & 0xffff0000u) | sp;
}

uint16_t v86_pop16(struct frame *f)
{
  uint16_t sp = (uint16_t)f->esp;
  uint16_t v = *(uint16_t *)rm_ptr(f->ss, sp);
  f->esp = (f->esp & 0xffff0000u) | (uint16_t)(sp + 2);
  return v;
}

static uint32_t pop32(struct frame *f)
{
  uint16_t sp = (uint16_t)f->esp;
  uint32_t v = *(uint32_t *)rm_ptr(f->ss, sp);
  f->esp = (f->esp & 0xffff0000u) | (uint16_t)(sp + 4);
  return v;
}

/* ---------------------------------------------------------------- */
/* virtual flags                                                    */

/* flags as the V86 program sees them */
uint32_t v86_vflags(struct frame *f)
{
  return (f->eflags & (EFL_USER | EFL_AC | EFL_ID)) | 0x2 |
      (vm1.vif ? EFL_IF : 0) | vm1.vflags;
}

static void vflags_set(struct frame *f, uint32_t v, int size32)
{
  uint32_t mask = EFL_USER | (size32 ? EFL_AC | EFL_ID : 0);

  f->eflags = (f->eflags & ~mask) | (v & mask) | EFL_IF | EFL_VM;
  vm1.vif = (v & EFL_IF) != 0;
  vm1.vflags = v & (EFL_IOPL | EFL_NT);
}

static void set_cf(struct frame *f, int on)
{
  if (on)
    f->eflags |= EFL_CF;
  else
    f->eflags &= ~EFL_CF;
}

/* ---------------------------------------------------------------- */
/* interrupt reflection                                             */

/* simulate a real mode INT vec: f->eip must point at the return address */
void v86_reflect(struct frame *f, uint8_t vec)
{
  const uint16_t *ivt = (const uint16_t *)(uint32_t)(vec * 4);

  v86_push16(f, (uint16_t)v86_vflags(f));
  v86_push16(f, (uint16_t)f->cs);
  v86_push16(f, (uint16_t)f->eip);
  vm1.vif = 0;
  f->eflags &= ~(EFL_TF | EFL_AC);
  f->eip = ivt[0];
  f->cs = ivt[1];
}

static uint8_t irq_vector(int irq)
{
  return irq < 8 ? 0x08 + irq : 0x70 + irq - 8;
}

/* deliver the highest priority pending IRQ if the VM accepts interrupts */
void v86_deliver(struct frame *f)
{
  static const uint8_t prio[16] = { 0, 1, 8, 9, 10, 11, 12, 13, 14, 15, 3, 4, 5, 6, 7, 2 };
  int i;

  if (!vm1.vif || !vm1.pending)
    return;
  for (i = 0; i < 16; i++)
  {
    int irq = prio[i];
    if (vm1.pending & (1u << irq))
    {
      vm1.pending &= ~(1u << irq);
      if (!dpmi_irq_v86(f, irq))
        v86_reflect(f, irq_vector(irq));
      return;
    }
  }
}

void v86_irq(struct frame *f, int irq)
{
  vm1.pending |= 1u << irq;
  /* from the monitor itself (HLT wait): just record it */
  if (!(f->eflags & EFL_VM))
    return;
  if (irq == 1 && vm_kbd(f))
    return;
  if (irq == 0)
    vm_tick(f);                 /* may switch sessions */
  v86_deliver(f);
}

/* emulate HLT: wait for an interrupt the VM will accept */
static void halt(struct frame *f)
{
  vm_yield(f);                  /* another session may run instead */
  while (!(vm1.vif && vm1.pending))
    __asm__ volatile("sti; hlt; cli" ::: "memory");
  v86_deliver(f);
}

/* ---------------------------------------------------------------- */
/* intercepted software interrupts                                  */

/* physical address of a block move address: the V86 address space (first
   1MB + 64K) is paged (private session memory), extended memory is not */
static uint32_t move_phys(uint32_t a)
{
  if (a >= 0x110000)
    return a;
  return (lin_to_pte(a) & ~0xfffu) | (a & 0xfff);
}

/* INT 15h AH=87h: move extended memory block (used by XMS drivers) */
static void int15_move(struct frame *f)
{
  const uint8_t *gdt = rm_ptr(f->es, SI(f));
  uint32_t src = gdt[0x12] | gdt[0x13] << 8 | gdt[0x14] << 16 | (uint32_t)gdt[0x17] << 24;
  uint32_t dst = gdt[0x1a] | gdt[0x1b] << 8 | gdt[0x1c] << 16 | (uint32_t)gdt[0x1f] << 24;
  uint32_t len = (uint32_t)CX(f) * 2;

  if (len > 0x10000)
  {
    AH(f) = 0x01;
    set_cf(f, 1);
    return;
  }
  while (len)
  {
    uint32_t n = 4096 - (src & 0xfff);
    if (n > 4096 - (dst & 0xfff))
      n = 4096 - (dst & 0xfff);
    if (n > len)
      n = len;
    phys_copy(move_phys(dst), move_phys(src), n);
    src += n;
    dst += n;
    len -= n;
  }
  AH(f) = 0;
  set_cf(f, 0);
  f->eflags |= EFL_ZF;
}

/* returns 1 if handled by the monitor, 0 to reflect to the VM */
static int intercept(struct frame *f, uint8_t vec)
{
  if (vec == 0x15)
  {
    if (AH(f) == 0x87)
    {
      int15_move(f);
      return 1;
    }
    if (AH(f) == 0x89)          /* switch to protected mode: refuse */
    {
      AH(f) = 0xff;
      set_cf(f, 1);
      return 1;
    }
  }
  else if (vec == 0x2f && AX(f) == 0x1687)
  {
    dpmi_detect(f);
    return 1;
  }
  else if (vec == 0x21 && AH(f) == 0x4c)
    dpmi_rm_exit();             /* then terminate in DOS as usual */
  else if (vec == 0x2a)
    vm_crit(f);                 /* then reflect */
  else if (vec == 0x28 || (vec == 0x2f && AX(f) == 0x1680))
    vm_yield(f);                /* DOS idle: let other sessions run */
  else if (vec == 0x2f && AH(f) == 0xde)
  {
    if (vm_api(f))
      return 1;
    switch (AL(f))
    {
      case 0x00:                /* installation check */
        AL(f) = 0xff;
        BX(f) = 0x4d44;         /* "MD" */
        CX(f) = 0x0001;         /* API version */
        return 1;
      case 0x01:                /* log ASCIZ string at DS:DX */
      {
        const char *s = rm_ptr(f->ds, DX(f));
        int n;
        for (n = 0; n < 256 && s[n]; n++)
          kprintf("%c", s[n]);
        return 1;
      }
    }
  }
  return 0;
}

/* ---------------------------------------------------------------- */
/* port I/O                                                         */

static int kbc_output_port;      /* next 60h write is KBC output port data */

static uint32_t io_in(uint16_t port, int size)
{
  uint32_t v;
  uint8_t b;

  if (size == 1 && dma_port(port))
    return dma_in(port);
  if (port == 0x60 && size == 1 && vm_port60(&b))
    return b;                   /* keystroke replayed by the session code */
  v = size == 1 ? inb(port) : size == 2 ? inw(port) : inl(port);

  if (port == 0x92)
    v |= 0x02;                  /* A20 reported (and kept) on */
  return v;
}

static void io_out(uint16_t port, int size, uint32_t v)
{
  if (size == 1 && dma_port(port))
  {
    dma_out(port, (uint8_t)v);
    return;
  }
  switch (port)
  {
    case 0x92:
      v |= 0x02;                /* never disable A20; bit 0 still resets */
      break;
    case 0x64:
      if ((v & 0xff) == 0xd1)   /* write output port: swallow with data */
      {
        kbc_output_port = 1;
        return;
      }
      if ((v & 0xff) == 0xdd || (v & 0xff) == 0xdf)
        return;                 /* A20 disable/enable commands */
      break;
    case 0x60:
      if (kbc_output_port)
      {
        kbc_output_port = 0;
        return;
      }
      break;
  }
  if (size == 1)
    outb(port, (uint8_t)v);
  else if (size == 2)
    outw(port, (uint16_t)v);
  else
    outl(port, v);
}

static void io_reg_in(struct frame *f, uint16_t port, int size)
{
  uint32_t v = io_in(port, size);
  if (size == 1)
    AL(f) = (uint8_t)v;
  else if (size == 2)
    AX(f) = (uint16_t)v;
  else
    f->eax = v;
}

/* ---------------------------------------------------------------- */
/* #GP: emulate the sensitive instruction at CS:IP                  */

static void v86_gp(struct frame *f)
{
  uint16_t ip = (uint16_t)f->eip;
  int o32 = 0, len = 0;
  uint8_t op;

  /* prefixes */
  for (;;)
  {
    op = fetch8(f, ip + len);
    if (op == 0x66)
      o32 = 1;
    else if (!(op == 0x67 || op == 0xf2 || op == 0xf3 || op == 0xf0 ||
               op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e ||
               op == 0x64 || op == 0x65))
      break;
    if (++len > 14)
      panic_frame(f, "too many prefixes");
  }
  len++;

  switch (op)
  {
    case 0xfa:                  /* CLI */
      f->eip = (uint16_t)(ip + len);
      vm1.vif = 0;
      return;
    case 0xfb:                  /* STI */
      f->eip = (uint16_t)(ip + len);
      vm1.vif = 1;
      v86_deliver(f);
      vm_poll(f);
      return;
    case 0x9c:                  /* PUSHF(D) */
      f->eip = (uint16_t)(ip + len);
      if (o32)
        push32(f, v86_vflags(f));
      else
        v86_push16(f, (uint16_t)v86_vflags(f));
      return;
    case 0x9d:                  /* POPF(D) */
      f->eip = (uint16_t)(ip + len);
      vflags_set(f, o32 ? pop32(f) : v86_pop16(f), o32);
      v86_deliver(f);
      vm_poll(f);
      return;
    case 0xcf:                  /* IRET(D) */
      if (o32)
      {
        f->eip = pop32(f) & 0xffff;
        f->cs = pop32(f) & 0xffff;
        vflags_set(f, pop32(f), 1);
      }
      else
      {
        f->eip = v86_pop16(f);
        f->cs = v86_pop16(f);
        vflags_set(f, v86_pop16(f), 0);
      }
      v86_deliver(f);
      vm_poll(f);
      return;
    case 0xcd:                  /* INT n */
    {
      uint8_t vec = fetch8(f, ip + len);
      f->eip = (uint16_t)(ip + len + 1);
      if (!intercept(f, vec))
        v86_reflect(f, vec);
      return;
    }
    case 0xcc:                  /* INT3 */
      f->eip = (uint16_t)(ip + len);
      v86_reflect(f, 3);
      return;
    case 0xce:                  /* INTO */
      f->eip = (uint16_t)(ip + len);
      if (f->eflags & EFL_OF)
        v86_reflect(f, 4);
      return;
    case 0xf4:                  /* HLT (or a DPMI breakpoint) */
      if (f->cs == boot.stub_seg && ip >= boot.stub_bp && ip < boot.stub_bp + BP_COUNT)
      {
        dpmi_v86_bp(f, ip - boot.stub_bp);
        return;
      }
      if (bios_bp(f, ip))
        return;
      f->eip = (uint16_t)(ip + len);
      halt(f);
      return;
    case 0xe4:                  /* IN AL/AX, imm8 */
    case 0xe5:
      f->eip = (uint16_t)(ip + len + 1);
      io_reg_in(f, fetch8(f, ip + len), op == 0xe4 ? 1 : o32 ? 4 : 2);
      return;
    case 0xe6:                  /* OUT imm8, AL/AX */
    case 0xe7:
      f->eip = (uint16_t)(ip + len + 1);
      io_out(fetch8(f, ip + len), op == 0xe6 ? 1 : o32 ? 4 : 2, f->eax);
      return;
    case 0xec:                  /* IN AL/AX, DX */
    case 0xed:
      f->eip = (uint16_t)(ip + len);
      io_reg_in(f, DX(f), op == 0xec ? 1 : o32 ? 4 : 2);
      return;
    case 0xee:                  /* OUT DX, AL/AX */
    case 0xef:
      f->eip = (uint16_t)(ip + len);
      io_out(DX(f), op == 0xee ? 1 : o32 ? 4 : 2, f->eax);
      return;
  }
  panic_frame(f, "unhandled #GP in V86 mode");
}

/* ---------------------------------------------------------------- */

void v86_exception(struct frame *f)
{
  switch (f->vector)
  {
    case 0x0d:
      v86_gp(f);
      return;
    case 0x00:                  /* divide error */
    case 0x01:                  /* debug / single step */
    case 0x02:                  /* NMI */
    case 0x04:                  /* overflow */
    case 0x05:                  /* bound */
    case 0x06:                  /* invalid opcode */
    case 0x07:                  /* no FPU */
      v86_reflect(f, (uint8_t)f->vector);
      return;
  }
  panic_frame(f, "exception in V86 mode");
}

/* first entry into V86: resume the loader where it switched to us */
void v86_start(void)
{
  static struct frame start;     /* not on the stack: enter_frame copies it there */
  struct frame *f = &start;

  memset(f, 0, sizeof(*f));
  f->eip = boot.ip;
  f->cs = boot.cs;
  f->esp = boot.sp;
  f->ss = boot.ss;
  f->ds = boot.ds;
  f->es = boot.es;
  f->fs = boot.fs;
  f->gs = boot.gs;
  f->eflags = EFL_VM | EFL_IF | 0x2 | (boot.eflags & EFL_USER);
  vm1.vif = boot.boot_type == BOOT_UEFI; /* the DOS loader executes STI */
  vm1.pending = 0;
  kprintf("v86: entering VM1 at %04x:%04x\n", boot.cs, boot.ip);
  enter_frame(f);
}
