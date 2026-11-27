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
      (vm1.vif ? EFL_IF : 0) | EFL_IOPL | (vm1.vflags & EFL_NT);
}

static void vflags_set(struct frame *f, uint32_t v, int size32)
{
  uint32_t mask = EFL_USER | (size32 ? EFL_AC | EFL_ID : 0);

  f->eflags = (f->eflags & ~mask) | (v & mask) | EFL_IF | EFL_VM;
  vm1.vif = (v & EFL_IF) != 0;
  /* The I/O privilege level a program sees is 3 and stays 3, as under the
     V86 monitors DOS programs know (EMM386, Windows): that it cannot be
     changed is how programs tell V86 mode from real mode (DOS/32A; with a
     changeable one it took the machine for real mode and tried to switch
     to protected mode itself). */
  vm1.vflags = v & EFL_NT;
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

/* Reflect an interrupt so that the monitor hears of the handler's return:
   below the handler's return frame (to breakpoint bp in the stub) lies the
   real one, which v86_bp_return takes. Nothing but the stack keeps the
   way back, so this nests and survives a handler that never returns. */
void v86_reflect_bp(struct frame *f, uint8_t vec, int bp)
{
  v86_reflect(f, vec);
  v86_push16(f, (uint16_t)v86_vflags(f));
  v86_push16(f, boot.stub_seg);
  v86_push16(f, (uint16_t)(boot.stub_bp + bp));
}

/* at that breakpoint: back to the interrupted code. hw: a hardware
   interrupt (all flags as they were); else a software interrupt (the
   handler's flags are its result, only the interrupt flag is restored). */
void v86_bp_return(struct frame *f, int hw)
{
  uint16_t fl;

  f->eip = v86_pop16(f);
  f->cs = v86_pop16(f);
  fl = v86_pop16(f);
  if (hw)
    vflags_set(f, fl, 0);
  else
    vm1.vif = (fl & EFL_IF) != 0;
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

  if (vm1.vif && !vm1.pending && vm_owed_tick())
  {
    /* a tick it missed: to a DPMI client's own handler if it has one,
       else to the real mode one; the monitor hears of the return */
    if (!dpmi_irq_v86(f, 0))
      v86_reflect_bp(f, 0x08, BP_TICK);
    return;
  }
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
  /* the keyboard: the monitor takes the scancode (also while it waits in
     HLT for the VM: then the keystroke is queued) */
  if (irq == 1 && vm_kbd(f, (f->eflags & EFL_VM) != 0))
    return;
  vm1.pending |= 1u << irq;
  /* from the monitor itself (HLT wait): just record it */
  if (!(f->eflags & EFL_VM))
    return;
  if (irq == 0)
    vm_tick(f);                 /* may switch sessions */
  v86_deliver(f);
}

/* emulate HLT: wait for an interrupt the VM will accept */
static void halt(struct frame *f)
{
  if (vm1.vif && vm_key_pending())
  {
    vm_poll(f);                 /* a queued keystroke wakes it up */
    return;
  }
  {
    uint16_t at = (uint16_t)f->eip, cs = (uint16_t)f->cs;
    vm_yield(f);                /* another session may run instead */
    if ((uint16_t)f->eip != at || (uint16_t)f->cs != cs)
      return;                   /* it waited itself and an interrupt came: its handler runs */
  }
  while (!(vm1.vif && (vm1.pending || vm_key_pending())))
    __asm__ volatile("sti; hlt; cli" ::: "memory");
  v86_deliver(f);
  vm_poll(f);                   /* a keystroke that came meanwhile */
}

/* ---------------------------------------------------------------- */
/* intercepted software interrupts                                  */

/* physical address of a block move address: the V86 address space (first
   1MB + 64K) is paged (private session memory); extended memory goes
   through ext_phys (UEFI boots). 0 if invalid. */
static uint32_t move_phys(uint32_t a)
{
  if (a >= 0x110000)
    return ext_phys(a);
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
    uint32_t pd = move_phys(dst), ps = move_phys(src);
    if (!pd || !ps)
    {
      AH(f) = 0x02;                     /* outside extended memory */
      set_cf(f, 1);
      return;
    }
    phys_copy(pd, ps, n);
    src += n;
    dst += n;
    len -= n;
  }
  AH(f) = 0;
  set_cf(f, 0);
  f->eflags |= EFL_ZF;
}

/* returns 1 if handled by the monitor, 0 to reflect to the VM */
/* Interrupt vectors a program sets and does not put back. DOS restores
   only INT 22h-24h when a program ends. A DOS extender that finds a DPMI
   host may leave the vector it took in real mode before it switched
   (DOS/4GW: INT 15h) - its own end-of-program code does not get to run in
   real mode under a host - and the next program then calls into freed
   memory. So the monitor notes what a vector was before a program first
   set it, and when the program ends (not: stays resident) puts back those
   that still point into the program's own memory. */
#define NVECS 96
static struct { uint16_t psp; uint8_t vec, used; uint32_t old; } vecs[NVECS];

void v86_vector_set(int psp, uint8_t vec)
{
  int i, slot = -1;

  if (psp <= 0)
    return;
  for (i = 0; i < NVECS; i++)
  {
    if (vecs[i].used && vecs[i].psp == psp && vecs[i].vec == vec)
      return;                           /* the first value is the one to keep */
    if (!vecs[i].used && slot < 0)
      slot = i;
  }
  if (slot < 0)
    return;
  vecs[slot].used = 1;
  vecs[slot].psp = (uint16_t)psp;
  vecs[slot].vec = vec;
  vecs[slot].old = *(const uint32_t *)(uint32_t)(vec * 4);
}

void v86_vectors_end(int psp, int resident)
{
  uint32_t *ivt = (uint32_t *)0, first, end;
  int i;

  if (psp <= 0)
    return;
  /* the program's memory block: from its PSP, the size in the MCB before it */
  first = (uint32_t)psp;
  end = first + *(const uint16_t *)rm_ptr((uint16_t)(psp - 1), 3);
  for (i = 0; i < NVECS; i++)
    if (vecs[i].used && vecs[i].psp == psp)
    {
      uint32_t seg = ivt[vecs[i].vec] >> 16;
      vecs[i].used = 0;
      if (!resident && seg >= first && seg < end && vecs[i].vec != 0x22 && vecs[i].vec != 0x23 &&
          vecs[i].vec != 0x24)
      {
        kprintf("v86: INT %02xh was left pointing into the program that ended: put back\n", vecs[i].vec);
        ivt[vecs[i].vec] = vecs[i].old;
      }
    }
}

static int intercept(struct frame *f, uint8_t vec)
{
  if (vec == 0x21 && AH(f) == 0x25)
    v86_vector_set(dos_current_psp(), AL(f));
  else if ((vec == 0x21 && (AH(f) == 0x4c || AH(f) == 0x00)) || vec == 0x20)
  {
    v86_vectors_end(dos_current_psp(), 0);
    bios_program_end();
  }
  else if ((vec == 0x21 && AH(f) == 0x31) || vec == 0x27)
    v86_vectors_end(dos_current_psp(), 1);

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
    if (AL(f) >= 0x10 && AL(f) <= 0x1f)
    {
      nic_api(f);               /* network (nic.c) */
      return 1;
    }
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

/* the monitor's services for an interrupt that is not an INT instruction
   in V86 mode (the DPMI host reflecting one from protected mode) */
int v86_intercept(struct frame *f, uint8_t vec)
{
  return intercept(f, vec);
}

/* ---------------------------------------------------------------- */
/* port I/O                                                         */

static uint16_t disk_call_ax, disk_call_dx;      /* the last INT 13h call reflected */
static int kbc_output_port;      /* next 60h write is KBC output port data */

static uint32_t io_in(uint16_t port, int size)
{
  uint32_t v;
  uint8_t b;

  if (size == 1 && dma_port(port))
    return dma_in(port);
  if (port == 0x60 && size == 1 && vm_port60(&b))
    return b;                   /* keystroke replayed by the session code */
  if (vga_port_in(port, size, &v))
    return v;                   /* the monitor's VGA (mode 13h on a UEFI boot) */
  v = size == 1 ? inb(port) : size == 2 ? inw(port) : inl(port);

  /* a scancode the monitor has taken from the keyboard controller waits
     for the session: the controller's status must say so (a BIOS that
     looks there first - SeaBIOS - else drops the key, and the scancode
     would later be read in place of a mouse byte) */
  if (port == 0x64 && size == 1 && vm_port60_waiting())
    v = (v | 0x01) & ~0x20u;
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
  if (port >= 0x3b0 && port <= 0x3df && vm_vga_blocked())
    return;                     /* not on the display: the hardware is not its */
  if (vga_port_out(port, size, v))
    return;
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
      if (((v & 0xff) == 0xad || (v & 0xff) == 0xae) && vm_kbd_owned())
        return;                 /* keyboard off/on, as BIOS keyboard handlers
                                   do around their work: the keyboard side of
                                   the controller is the monitor's (vm_kbd) */
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
      {
        if (vec == 0x10)
        {
          int v = vm_video_int(f);
          if (v == 1)
            return;             /* done for a session that is not on the display */
          if (v == 2)
          {
            /* it needs the display: wait for it, then ask again */
            f->eip = ip;
            vm1.vif = 1;
            halt(f);
            return;
          }
        }
        if (vec == 0x16 && vm_key_wait(f))
        {
          /* no key yet: wait here, not in the BIOS, and ask again */
          f->eip = ip;
          vm1.vif = 1;
          halt(f);
          return;
        }
        if (vm_bios_call(vec))
        {
          if (vec == 0x13)
          {
            disk_call_ax = AX(f);       /* for the log, should it fail */
            disk_call_dx = DX(f);
          }
          v86_reflect_bp(f, vec, BP_INT13);
        }
        else
          v86_reflect(f, vec);
      }
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
        if (ip - boot.stub_bp == BP_INT13)
        {
          /* back from a BIOS service. As at an IRET: interrupts that came
             while the handler had them off are due now. (Without this a
             timer interrupt that arrived inside the BIOS stayed pending -
             and in service at the interrupt controller, so no other came
             - until the program's next IRET or STI: a loop that only reads
             the time through the BIOS waited forever.) */
          v86_bp_return(f, 0);
          vm_bios_done();
          /* a failed disk call of the machine's BIOS is worth a line in the
             log (not the questions programs ask it: drive parameters, ...) */
          {
            uint8_t fn = (uint8_t)(disk_call_ax >> 8);
            if ((fn == 2 || fn == 3 || fn == 0x42 || fn == 0x43) && ((f->eflags & EFL_CF) || (AX(f) >> 8)))
              kprintf("bios: INT 13h AX=%04x DL=%02x failed: AX=%04x, carry %u\n", disk_call_ax,
                      disk_call_dx & 0xff, AX(f), (unsigned)(f->eflags & EFL_CF));
          }
          disk_call_ax = 0;
          v86_deliver(f);
          vm_poll(f);
        }
        else if (ip - boot.stub_bp == BP_TICK)
        {
          v86_bp_return(f, 1);
          vm_tick_done();
          v86_deliver(f);
          vm_poll(f);
        }
        else if (ip - boot.stub_bp == BP_KBD)
        {
          v86_bp_return(f, 1);
          vm_kbd_done();
          v86_deliver(f);
          vm_poll(f);                   /* with the next keystroke, if one waits */
        }
        else
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
    case 0x0f:                  /* system instructions */
    {
      uint8_t op2 = fetch8(f, ip + len), modrm = fetch8(f, ip + len + 1);
      uint32_t *regs[8] = { &f->eax, &f->ecx, &f->edx, &f->ebx, &f->esp, &f->ebp, &f->esi, &f->edi };
      uint32_t *r = regs[modrm & 7];
      int n = (modrm >> 3) & 7;

      if (op2 == 0x20 && (modrm & 0xc0) == 0xc0)
      {
        /* MOV r32, CRn: DOS extenders look at CR0 to see that the machine
           is in protected mode (V86) before they ask for DPMI. They get
           that, without the paging bit, and nothing of CR2-CR4. */
        uint32_t cr0;
        __asm__("mov %%cr0, %0" : "=r"(cr0));
        *r = n == 0 ? (cr0 & 0x0000003fu) | 1 : 0;
        f->eip = (uint16_t)(ip + len + 2);
        return;
      }
      if (op2 == 0x21 && (modrm & 0xc0) == 0xc0)
      {
        *r = 0;                 /* MOV r32, DRn */
        f->eip = (uint16_t)(ip + len + 2);
        return;
      }
      if ((op2 == 0x22 || op2 == 0x23) && (modrm & 0xc0) == 0xc0)
      {
        /* MOV CRn/DRn, r32: not the program's to set. Cache control and
           the like in CR0 are ignored; clearing the protected mode bit (a
           switch to real mode) cannot be done: the program is ended. */
        if (op2 == 0x22 && n == 0 && !(*r & 1))
          break;
        f->eip = (uint16_t)(ip + len + 2);
        return;
      }
      if (op2 == 0x06 || op2 == 0x08 || op2 == 0x09)
      {
        f->eip = (uint16_t)(ip + len + 1);      /* CLTS, INVD, WBINVD: nothing */
        return;
      }
      break;
    }
  }
  /* An instruction a DOS program cannot use under the monitor (a switch
     to protected mode of its own, ...): the program is ended, as by Ctrl+C,
     not the machine - unless that does not help. */
  {
    static uint32_t last_cs, last_ip, again;
    if (last_cs == f->cs && last_ip == f->eip && ++again >= 3)
      panic_frame(f, "unhandled #GP in V86 mode");
    if (last_cs != f->cs || last_ip != f->eip)
      again = 0;
    last_cs = f->cs;
    last_ip = f->eip;
    kprintf("v86: a program used an instruction it cannot use here at %04x:%04x (%02x %02x %02x %02x %02x): ended\n",
            f->cs, ip, fetch8(f, ip), fetch8(f, ip + 1), fetch8(f, ip + 2), fetch8(f, ip + 3), fetch8(f, ip + 4));
    dpmi_rm_exit();
    AX(f) = 0x4cff;
    vm1.vif = 1;
    v86_reflect(f, 0x21);
  }
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
