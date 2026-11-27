/* MDVMM core - the instructions that reach VGA memory, run by the monitor
 *
 * In the planar VGA modes one byte written at A000:xxxx goes to up to four
 * memory planes, through the card's write modes and latches (vga.c). The
 * monitor leaves A0000h unmapped there; a program's access faults, and the
 * instruction is done here instead: the usual ones for video memory - MOV,
 * the string instructions with REP, the logical and arithmetic ones, TEST,
 * XCHG, INC/DEC, NOT/NEG, MOVZX/MOVSX - in 16 and 32 bit code, V86 mode
 * and DPMI clients. A REP runs to its end in one go.
 */
#include "core.h"

#define VGA_LO 0xA0000u
#define VGA_HI 0xB0000u

struct ctx {
  struct frame *f;
  int v86, op32, addr32;
  uint32_t ip;                          /* offset of the next byte to fetch */
  uint32_t cs_base;
  int seg_override;                     /* -1, or 0 ES 1 CS 2 SS 3 DS 4 FS 5 GS */
  int rep;                              /* 0, 0xf3, 0xf2 */
};

static uint32_t seg_base(struct ctx *c, int seg)
{
  struct frame *f = c->f;
  uint32_t sel;

  if (c->v86)
  {
    switch (seg)
    {
      case 0: sel = f->es; break;
      case 1: sel = f->cs; break;
      case 2: sel = f->ss; break;
      case 3: sel = f->ds; break;
      case 4: sel = f->fs; break;
      default: sel = f->gs; break;
    }
    return (sel & 0xffff) << 4;
  }
  switch (seg)
  {
    case 0: sel = f->pes; break;
    case 1: sel = f->cs; break;
    case 2: sel = f->ss; break;
    case 3: sel = f->pds; break;
    case 4: sel = f->pfs; break;
    default: sel = f->pgs; break;
  }
  return dpmi_sel_base((uint16_t)sel);
}

static uint8_t rd8(uint32_t lin)
{
  if (lin >= VGA_LO && lin < VGA_HI)
    return vga_mem_read(lin - VGA_LO);
  return *(uint8_t *)lin;
}

static void wr8(uint32_t lin, uint8_t v)
{
  if (lin >= VGA_LO && lin < VGA_HI)
    vga_mem_write(lin - VGA_LO, v);
  else
    *(uint8_t *)lin = v;
}

static uint32_t rd(uint32_t lin, int size)
{
  uint32_t v = rd8(lin);

  if (size > 1)
    v |= (uint32_t)rd8(lin + 1) << 8;
  if (size > 2)
    v |= (uint32_t)rd8(lin + 2) << 16 | (uint32_t)rd8(lin + 3) << 24;
  return v;
}

static void wr(uint32_t lin, int size, uint32_t v)
{
  wr8(lin, (uint8_t)v);
  if (size > 1)
    wr8(lin + 1, (uint8_t)(v >> 8));
  if (size > 2)
  {
    wr8(lin + 2, (uint8_t)(v >> 16));
    wr8(lin + 3, (uint8_t)(v >> 24));
  }
}

static uint8_t fetch(struct ctx *c)
{
  uint32_t ip = c->v86 ? (c->ip & 0xffff) : c->ip;

  c->ip++;
  return *(uint8_t *)(c->cs_base + ip);
}

static uint32_t fetch_n(struct ctx *c, int size)
{
  uint32_t v = fetch(c);

  if (size > 1)
    v |= (uint32_t)fetch(c) << 8;
  if (size > 2)
  {
    v |= (uint32_t)fetch(c) << 16;
    v |= (uint32_t)fetch(c) << 24;
  }
  return v;
}

static uint32_t *reg32(struct ctx *c, int n)
{
  struct frame *f = c->f;
  uint32_t *r[8] = { &f->eax, &f->ecx, &f->edx, &f->ebx, &f->esp, &f->ebp, &f->esi, &f->edi };

  return r[n & 7];
}

static uint32_t reg_get(struct ctx *c, int n, int size)
{
  if (size == 1)
    return n < 4 ? *(uint8_t *)reg32(c, n) : *((uint8_t *)reg32(c, n - 4) + 1);
  return size == 2 ? *(uint16_t *)reg32(c, n) : *reg32(c, n);
}

static void reg_set(struct ctx *c, int n, int size, uint32_t v)
{
  if (size == 1)
  {
    if (n < 4)
      *(uint8_t *)reg32(c, n) = (uint8_t)v;
    else
      *((uint8_t *)reg32(c, n - 4) + 1) = (uint8_t)v;
  }
  else if (size == 2)
    *(uint16_t *)reg32(c, n) = (uint16_t)v;
  else
    *reg32(c, n) = v;
}

/* the operand of a ModRM byte: a register (*is_reg, its number in *lin) or
   the linear address of memory */
static void modrm(struct ctx *c, uint8_t m, int *is_reg, uint32_t *lin)
{
  int mod = m >> 6, rm = m & 7, seg = 3;
  uint32_t ea = 0;

  if (mod == 3)
  {
    *is_reg = 1;
    *lin = (uint32_t)rm;
    return;
  }
  *is_reg = 0;
  if (!c->addr32)
  {
    uint16_t bx = (uint16_t)c->f->ebx, bp = (uint16_t)c->f->ebp, si = (uint16_t)c->f->esi,
             di = (uint16_t)c->f->edi;
    switch (rm)
    {
      case 0: ea = bx + si; break;
      case 1: ea = bx + di; break;
      case 2: ea = bp + si; seg = 2; break;
      case 3: ea = bp + di; seg = 2; break;
      case 4: ea = si; break;
      case 5: ea = di; break;
      case 6:
        if (mod == 0)
          ea = fetch_n(c, 2);
        else
        {
          ea = bp;
          seg = 2;
        }
        break;
      default: ea = bx; break;
    }
    if (mod == 1)
      ea += (uint32_t)(int8_t)fetch(c);
    else if (mod == 2)
      ea += fetch_n(c, 2);
    ea &= 0xffff;
  }
  else
  {
    if (rm == 4)
    {
      uint8_t sib = fetch(c);
      int base = sib & 7, index = (sib >> 3) & 7;
      if (base == 5 && mod == 0)
        ea = fetch_n(c, 4);
      else
      {
        ea = *reg32(c, base);
        if (base == 4 || base == 5)
          seg = 2;
      }
      if (index != 4)
        ea += *reg32(c, index) << (sib >> 6);
    }
    else if (rm == 5 && mod == 0)
      ea = fetch_n(c, 4);
    else
    {
      ea = *reg32(c, rm);
      if (rm == 5)
        seg = 2;
    }
    if (mod == 1)
      ea += (uint32_t)(int8_t)fetch(c);
    else if (mod == 2)
      ea += fetch_n(c, 4);
  }
  *lin = seg_base(c, c->seg_override >= 0 ? c->seg_override : seg) + ea;
}

static uint32_t operand_get(struct ctx *c, int is_reg, uint32_t lin, int size)
{
  return is_reg ? reg_get(c, (int)lin, size) : rd(lin, size);
}

static void operand_set(struct ctx *c, int is_reg, uint32_t lin, int size, uint32_t v)
{
  if (is_reg)
    reg_set(c, (int)lin, size, v);
  else
    wr(lin, size, v);
}

/* the arithmetic and logical operations, with the flags the CPU gives:
   done by the CPU itself. op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR
   7 CMP 8 TEST 9 INC 10 DEC 11 NOT 12 NEG */
#define ALU1(insn, type) \
  do { type a_ = (type)a, b_ = (type)b; \
       __asm__("push %[fl]; popf; " insn "; pushf; pop %[fl]" : [a] "+q"(a_), [fl] "+r"(fl) : [b] "q"(b_) : "cc"); \
       a = a_; } while (0)
#define ALU(insn8, insn16, insn32) \
  do { if (size == 1) ALU1(insn8, uint8_t); else if (size == 2) ALU1(insn16, uint16_t); else ALU1(insn32, uint32_t); } while (0)

static uint32_t alu(struct ctx *c, int op, int size, uint32_t a, uint32_t b)
{
  uint32_t fl = (c->f->eflags & 0x8d5) | 0x2;      /* the arithmetic flags, interrupts off */

  switch (op)
  {
    case 0: ALU("addb %[b], %[a]", "addw %[b], %[a]", "addl %[b], %[a]"); break;
    case 1: ALU("orb %[b], %[a]", "orw %[b], %[a]", "orl %[b], %[a]"); break;
    case 2: ALU("adcb %[b], %[a]", "adcw %[b], %[a]", "adcl %[b], %[a]"); break;
    case 3: ALU("sbbb %[b], %[a]", "sbbw %[b], %[a]", "sbbl %[b], %[a]"); break;
    case 4: ALU("andb %[b], %[a]", "andw %[b], %[a]", "andl %[b], %[a]"); break;
    case 5: ALU("subb %[b], %[a]", "subw %[b], %[a]", "subl %[b], %[a]"); break;
    case 6: ALU("xorb %[b], %[a]", "xorw %[b], %[a]", "xorl %[b], %[a]"); break;
    case 7: { uint32_t keep = a; ALU("cmpb %[b], %[a]", "cmpw %[b], %[a]", "cmpl %[b], %[a]"); a = keep; break; }
    case 8: { uint32_t keep = a; ALU("testb %[b], %[a]", "testw %[b], %[a]", "testl %[b], %[a]"); a = keep; break; }
    case 9: ALU("incb %[a]", "incw %[a]", "incl %[a]"); break;
    case 10: ALU("decb %[a]", "decw %[a]", "decl %[a]"); break;
    case 11: a = ~a; return a;          /* NOT: no flags */
    default: ALU("negb %[a]", "negw %[a]", "negl %[a]"); break;
  }
  c->f->eflags = (c->f->eflags & ~0x8d5u) | (fl & 0x8d5);
  return a;
}

/* the string instructions: MOVS, STOS, LODS, with REP */
static void string_op(struct ctx *c, uint8_t op)
{
  struct frame *f = c->f;
  int size = (op & 1) ? (c->op32 ? 4 : 2) : 1;
  uint32_t mask = c->addr32 ? 0xffffffffu : 0xffffu, count = 1, n;
  uint32_t step = (f->eflags & 0x400) ? (uint32_t)-size : (uint32_t)size;
  uint32_t src_base = seg_base(c, c->seg_override >= 0 ? c->seg_override : 3), dst_base = seg_base(c, 0);
  uint32_t si = f->esi & mask, di = f->edi & mask;

  if (c->rep)
    count = f->ecx & mask;
  for (n = 0; n < count; n++)
  {
    switch (op & 0xfe)
    {
      case 0xa4:                        /* MOVS */
        wr(dst_base + di, size, rd(src_base + si, size));
        si = (si + step) & mask;
        di = (di + step) & mask;
        break;
      case 0xaa:                        /* STOS */
        wr(dst_base + di, size, f->eax);
        di = (di + step) & mask;
        break;
      default:                          /* LODS */
        reg_set(c, 0, size, rd(src_base + si, size));
        si = (si + step) & mask;
        break;
    }
  }
  if (c->rep)
    f->ecx = (f->ecx & ~mask);
  f->esi = (f->esi & ~mask) | si;
  f->edi = (f->edi & ~mask) | di;
}

/* The instruction at the frame's CS:EIP, which touched VGA memory. v86: a
   V86 frame; code32: the code segment is a 32 bit one. 1 if it was done;
   0: not one of those above (nothing is changed then). */
int vgaemu(struct frame *f, int v86, int code32)
{
  struct ctx c;
  struct frame saved;
  uint8_t op, m;
  uint32_t lin, a, b;
  int is_reg, size, n;

  memcpy(&saved, f, sizeof(saved));
  c.f = f;
  c.v86 = v86;
  c.op32 = c.addr32 = code32;
  c.ip = f->eip;
  c.seg_override = -1;
  c.rep = 0;
  c.cs_base = v86 ? (f->cs & 0xffff) << 4 : dpmi_sel_base((uint16_t)f->cs);

  for (n = 0; n < 8; n++)
  {
    op = fetch(&c);
    if (op == 0x66)
      c.op32 = !code32;
    else if (op == 0x67)
      c.addr32 = !code32;
    else if (op == 0x26)
      c.seg_override = 0;
    else if (op == 0x2e)
      c.seg_override = 1;
    else if (op == 0x36)
      c.seg_override = 2;
    else if (op == 0x3e)
      c.seg_override = 3;
    else if (op == 0x64)
      c.seg_override = 4;
    else if (op == 0x65)
      c.seg_override = 5;
    else if (op == 0xf3 || op == 0xf2)
      c.rep = op;
    else if (op != 0xf0)
      break;
  }
  size = (op & 1) ? (c.op32 ? 4 : 2) : 1;

  if (op >= 0xa4 && op <= 0xad && op != 0xa6 && op != 0xa7 && op != 0xa8 && op != 0xa9)
    string_op(&c, op);
  else if (op < 0x40 && (op & 7) < 4)   /* ALU r/m,r and r,r/m */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    if (op & 2)
    {
      a = reg_get(&c, (m >> 3) & 7, size);
      b = operand_get(&c, is_reg, lin, size);
      a = alu(&c, op >> 3, size, a, b);
      if ((op >> 3) != 7)
        reg_set(&c, (m >> 3) & 7, size, a);
    }
    else
    {
      a = operand_get(&c, is_reg, lin, size);
      b = reg_get(&c, (m >> 3) & 7, size);
      a = alu(&c, op >> 3, size, a, b);
      if ((op >> 3) != 7)
        operand_set(&c, is_reg, lin, size, a);
    }
  }
  else if (op == 0x80 || op == 0x81 || op == 0x83)      /* ALU r/m, imm */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    b = op == 0x81 ? fetch_n(&c, size) : (uint32_t)(int8_t)fetch(&c);
    a = alu(&c, (m >> 3) & 7, size, operand_get(&c, is_reg, lin, size), b);
    if (((m >> 3) & 7) != 7)
      operand_set(&c, is_reg, lin, size, a);
  }
  else if (op == 0x84 || op == 0x85)    /* TEST r/m, r */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    alu(&c, 8, size, operand_get(&c, is_reg, lin, size), reg_get(&c, (m >> 3) & 7, size));
  }
  else if (op == 0x86 || op == 0x87)    /* XCHG r/m, r */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    a = operand_get(&c, is_reg, lin, size);
    operand_set(&c, is_reg, lin, size, reg_get(&c, (m >> 3) & 7, size));
    reg_set(&c, (m >> 3) & 7, size, a);
  }
  else if (op == 0x88 || op == 0x89)    /* MOV r/m, r */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    operand_set(&c, is_reg, lin, size, reg_get(&c, (m >> 3) & 7, size));
  }
  else if (op == 0x8a || op == 0x8b)    /* MOV r, r/m */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    reg_set(&c, (m >> 3) & 7, size, operand_get(&c, is_reg, lin, size));
  }
  else if (op >= 0xa0 && op <= 0xa3)    /* MOV accumulator, moffs and back */
  {
    lin = seg_base(&c, c.seg_override >= 0 ? c.seg_override : 3) + fetch_n(&c, c.addr32 ? 4 : 2);
    if (op < 0xa2)
      reg_set(&c, 0, size, rd(lin, size));
    else
      wr(lin, size, f->eax);
  }
  else if (op == 0xc6 || op == 0xc7)    /* MOV r/m, imm */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    operand_set(&c, is_reg, lin, size, fetch_n(&c, size));
  }
  else if (op == 0xf6 || op == 0xf7)    /* TEST imm, NOT, NEG */
  {
    m = fetch(&c);
    modrm(&c, m, &is_reg, &lin);
    n = (m >> 3) & 7;
    if (n == 0 || n == 1)
      alu(&c, 8, size, operand_get(&c, is_reg, lin, size), fetch_n(&c, size));
    else if (n == 2 || n == 3)
      operand_set(&c, is_reg, lin, size, alu(&c, n == 2 ? 11 : 12, size, operand_get(&c, is_reg, lin, size), 0));
    else
      goto unknown;
  }
  else if (op == 0xfe || op == 0xff)    /* INC, DEC */
  {
    m = fetch(&c);
    n = (m >> 3) & 7;
    if (n > 1)
      goto unknown;                     /* CALL, JMP, PUSH through video memory: no */
    modrm(&c, m, &is_reg, &lin);
    operand_set(&c, is_reg, lin, size, alu(&c, n ? 10 : 9, size, operand_get(&c, is_reg, lin, size), 0));
  }
  else if (op == 0x0f)
  {
    uint8_t op2 = fetch(&c);
    if (op2 != 0xb6 && op2 != 0xb7 && op2 != 0xbe && op2 != 0xbf)
      goto unknown;
    m = fetch(&c);                      /* MOVZX, MOVSX */
    modrm(&c, m, &is_reg, &lin);
    a = operand_get(&c, is_reg, lin, (op2 & 1) ? 2 : 1);
    if (op2 & 8)
      a = (op2 & 1) ? (uint32_t)(int16_t)a : (uint32_t)(int8_t)a;
    reg_set(&c, (m >> 3) & 7, c.op32 ? 4 : 2, a);
  }
  else
    goto unknown;
  f->eip = v86 ? (c.ip & 0xffff) : c.ip;
  return 1;

unknown:
  memcpy(f, &saved, sizeof(saved));
  return 0;
}
