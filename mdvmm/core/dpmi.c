/* MDVMM core - DPMI 0.9 host
 *
 * One client at a time (16 or 32 bit), running at ring 3 with IOPL 3, so
 * CLI/STI/IRET act on the real interrupt flag. Transitions between the
 * client and real mode are driven by breakpoints:
 *   V86 side: HLT bytes in the loader's resident stub (BP_* in bootinfo.h)
 *   PM side:  a ring 3 host code page full of HLT (HBP_* below)
 * Nested transitions are kept on a small stack of saved frames.
 */
#include "core.h"

#define LDT_ENTRIES   1024
#define HOST_AREA     0xE0000000u       /* host code page + host PM stack */
#define HOST_STACK    0x5000u           /* top of host stack (HDATA offset) */
#define HOST_LEVEL    0x1000u           /* stack space per nesting level */

#define HBP_EXC_RET   0x000             /* exception handler returned (RETF) */
#define HBP_IRET      0x001             /* IRQ/callback handler returned (IRET) */
#define HBP_RAW       0x002             /* raw switch PM -> real mode */
#define HBP_RETF      0x010             /* RETF: PM state save no-op */
#define HBP_DEFINT    0x100             /* +vector: default PM int handlers */
#define HBP_DEFEXC    0x200             /* +exception: default handlers */

#define CLIENT_BASE   0x00400000u       /* client linear memory */
#define CLIENT_END    0xBF000000u
#define PHYSMAP_BASE  0xC0000000u       /* DPMI 0800h mappings */
#define PHYSMAP_END   0xDF000000u

#define MAX_BLOCKS    128
#define MAX_CALLBACKS 16
#define MAX_TRANS     16

#define ERR_UNSUPPORTED 0x8001
#define ERR_DESC        0x8011          /* descriptor unavailable */
#define ERR_LINEAR      0x8012
#define ERR_PHYSICAL    0x8013
#define ERR_CALLBACK    0x8015
#define ERR_HANDLE      0x8023
#define ERR_SELECTOR    0x8022
#define ERR_VALUE       0x8021

struct desc { uint16_t limit_lo, base_lo; uint8_t base_mid, access, gran, base_hi; } __attribute__((packed));
struct farptr { uint32_t off; uint16_t sel; };
struct block { uint32_t lin, size; };
struct callback { int used; struct farptr proc, regs; uint16_t stack_sel; };

struct rmregs {                         /* DPMI real mode call structure */
  uint32_t edi, esi, ebp, reserved, ebx, edx, ecx, eax;
  uint16_t flags, es, ds, fs, gs, ip, cs, sp, ss;
} __attribute__((packed));

enum {
  T_PM_INT,         /* PM software INT reflected to real mode      */
  T_PM_DEFINT,      /* client chained to the default PM handler    */
  T_PM_IRQ,         /* hardware IRQ during PM reflected to RM      */
  T_PM_CALL,        /* DPMI 0300h-0302h                            */
  T_PM_DOSMEM,      /* DPMI 0100h-0102h                            */
  T_TO_PM_HANDLER,  /* PM IRQ handler running (from PM or V86)     */
  T_CALLBACK,       /* real mode callback running in PM            */
};

struct trans {
  struct frame f;                       /* context to resume */
  int kind;
  uint32_t a, b, c;
  uint16_t rm_ss, rm_sp;
  uint32_t hstack;
};

static struct desc ldt[LDT_ENTRIES] __attribute__((aligned(8)));
static uint8_t ldt_used[LDT_ENTRIES];

static struct client {
  int active, bits32;
  uint16_t psp, env_seg, env_sel;
  uint16_t rm_ss, rm_sp;                /* host real mode stack */
  uint16_t rm_ds;                       /* segment for default reflection */
  struct farptr pmint[256];             /* sel 0: default */
  struct farptr exc[32];
  struct block blocks[MAX_BLOCKS];
  struct callback cb[MAX_CALLBACKS];
  uint32_t next_lin, next_phys;
} cl;

static struct trans ts[MAX_TRANS];
static int tsp;
static uint16_t rm_ss_cur, rm_sp_cur;   /* real mode stack for host calls */
static uint32_t hstack_cur = HOST_STACK;

/* ---------------------------------------------------------------- */
/* descriptors                                                      */

#define LSEL(i) ((uint16_t)(((i) << 3) | 7))

static uint32_t desc_base(const struct desc *d)
{
  return d->base_lo | (uint32_t)d->base_mid << 16 | (uint32_t)d->base_hi << 24;
}

static void desc_set_base(struct desc *d, uint32_t b)
{
  d->base_lo = b & 0xffff;
  d->base_mid = (b >> 16) & 0xff;
  d->base_hi = b >> 24;
}

static void desc_set_limit(struct desc *d, uint32_t l)
{
  if (l > 0xfffff)
  {
    d->gran |= 0x80;
    l >>= 12;
  }
  else
    d->gran &= ~0x80;
  d->limit_lo = l & 0xffff;
  d->gran = (d->gran & 0xf0) | ((l >> 16) & 0x0f);
}

static void desc_init(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
  ldt[i].gran = flags & 0x70;
  desc_set_base(&ldt[i], base);
  desc_set_limit(&ldt[i], limit);
  ldt[i].access = access;
}

static const struct desc *sel_desc(uint16_t sel)
{
  if (sel & 4)
    return (sel >> 3) < LDT_ENTRIES ? &ldt[sel >> 3] : &ldt[0];
  return gdt_entry(sel);
}

static uint32_t sel_base(uint16_t sel)
{
  return desc_base(sel_desc(sel));
}

static int sel_big(uint16_t sel)
{
  return (sel_desc(sel)->gran & 0x40) != 0;
}

static int sel_client(uint16_t sel)
{
  int i = sel >> 3;
  return (sel & 4) && i > 0 && i < LDT_ENTRIES && ldt_used[i];
}

static int ldt_alloc(int n)
{
  int i, j;

  for (i = 1; i + n <= LDT_ENTRIES; i++)
  {
    for (j = 0; j < n && !ldt_used[i + j]; j++)
      ;
    if (j < n)
    {
      i += j;
      continue;
    }
    for (j = 0; j < n; j++)
    {
      ldt_used[i + j] = 1;
      desc_init(i + j, 0, 0, 0xf2, cl.bits32 ? 0x40 : 0);
    }
    return i;
  }
  return -1;
}

static void ldt_free(int i)
{
  ldt_used[i] = 0;
  memset(&ldt[i], 0, sizeof(ldt[i]));
}

static void *pm_ptr(uint16_t sel, uint32_t off)
{
  return (void *)(sel_base(sel) + off);
}

/* offset registers: 32 bit for 32 bit clients */
static uint32_t off32(uint32_t r)
{
  return cl.bits32 ? r : (r & 0xffff);
}

/* ---------------------------------------------------------------- */
/* frames and stacks                                                */

static void pm_push(struct frame *f, uint32_t v, int size)
{
  uint32_t addr;

  if (sel_big(f->ss))
  {
    f->esp -= size;
    addr = sel_base(f->ss) + f->esp;
  }
  else
  {
    uint16_t sp = (uint16_t)(f->esp - size);
    f->esp = (f->esp & 0xffff0000u) | sp;
    addr = sel_base(f->ss) + sp;
  }
  if (size == 4)
    *(uint32_t *)addr = v;
  else
    *(uint16_t *)addr = (uint16_t)v;
}

static uint32_t pm_pop(struct frame *f, int size)
{
  uint32_t addr, v;

  if (sel_big(f->ss))
  {
    addr = sel_base(f->ss) + f->esp;
    f->esp += size;
  }
  else
  {
    uint16_t sp = (uint16_t)f->esp;
    addr = sel_base(f->ss) + sp;
    f->esp = (f->esp & 0xffff0000u) | (uint16_t)(sp + size);
  }
  v = size == 4 ? *(uint32_t *)addr : *(uint16_t *)addr;
  return v;
}

static int isize(void)
{
  return cl.bits32 ? 4 : 2;
}

/* client flags: IOPL 3, never VM/NT */
static uint32_t pm_flags(uint32_t fl)
{
  return (fl & ~(EFL_VM | EFL_NT | EFL_RF)) | EFL_IOPL | 0x2;
}

static void set_cf(struct frame *f, int on)
{
  if (on)
    f->eflags |= EFL_CF;
  else
    f->eflags &= ~EFL_CF;
}

static void api_error(struct frame *f, uint16_t code)
{
  AX(f) = code;
  set_cf(f, 1);
}

/* replace segment registers that no longer refer to client selectors */
static void sanitize_sregs(struct frame *f)
{
  if (f->pds && !sel_client(f->pds) && (f->pds & ~3) != (HDATA_SEL & ~3))
    f->pds = 0;
  if (f->pes && !sel_client(f->pes) && (f->pes & ~3) != (HDATA_SEL & ~3))
    f->pes = 0;
  if (f->pfs && !sel_client(f->pfs))
    f->pfs = 0;
  if (f->pgs && !sel_client(f->pgs))
    f->pgs = 0;
}

static void trans_push(const struct frame *f, int kind, uint32_t a, uint32_t b, uint32_t c)
{
  struct trans *t;

  if (tsp >= MAX_TRANS)
    panic("dpmi: transitions nested too deep");
  t = &ts[tsp++];
  memcpy(&t->f, f, sizeof(*f));
  t->kind = kind;
  t->a = a;
  t->b = b;
  t->c = c;
  t->rm_ss = rm_ss_cur;
  t->rm_sp = rm_sp_cur;
  t->hstack = hstack_cur;
}

static struct trans *trans_pop(void)
{
  struct trans *t;

  if (tsp == 0)
    panic("dpmi: unbalanced transition");
  t = &ts[--tsp];
  rm_ss_cur = t->rm_ss;
  rm_sp_cur = t->rm_sp;
  hstack_cur = t->hstack;
  return t;
}

/* V86 frame for a host initiated real mode call, registers from pm */
static void rm_setup(struct frame *v, const struct frame *pm)
{
  memset(v, 0, sizeof(*v));
  v->eax = pm->eax;
  v->ebx = pm->ebx;
  v->ecx = pm->ecx;
  v->edx = pm->edx;
  v->esi = pm->esi;
  v->edi = pm->edi;
  v->ebp = pm->ebp;
  v->ds = v->es = cl.rm_ds;
  v->ss = rm_ss_cur;
  v->esp = rm_sp_cur;
  v->eflags = EFL_VM | EFL_IF | 0x2 | (pm->eflags & EFL_USER & ~EFL_TF);
}

/* push a return to BP_RM_RETURN and enter the real mode code */
static void rm_go(struct frame *v, const struct frame *pm, int kind,
                  uint32_t a, uint32_t b, uint32_t c)
{
  trans_push(pm, kind, a, b, c);
  rm_sp_cur = (uint16_t)v->esp;
  rm_ss_cur = (uint16_t)v->ss;
  enter_frame(v);
}

static void rm_int(struct frame *v, uint8_t vec, uint32_t flags)
{
  const uint16_t *ivt = (const uint16_t *)(uint32_t)(vec * 4);

  v86_push16(v, (uint16_t)(flags | EFL_IF));
  v86_push16(v, boot.stub_seg);
  v86_push16(v, boot.stub_bp + BP_RM_RETURN);
  v->eip = ivt[0];
  v->cs = ivt[1];
  vm1.vif = 0;
}

/* run PM code `h` on the host stack, it returns with IRET to HBP_IRET */
static void pm_handler(const struct frame *cur, struct farptr h, int kind,
                       uint32_t a, uint32_t flags)
{
  struct frame n;
  uint32_t esp = hstack_cur;

  if (!(cur->eflags & EFL_VM) && cur->ss == HDATA_SEL && cur->esp - 0x100 < esp)
    esp = (cur->esp - 0x100) & ~3u;
  trans_push(cur, kind, a, 0, 0);
  hstack_cur = esp - HOST_LEVEL;

  memcpy(&n, cur, sizeof(n));
  if (cur->eflags & EFL_VM)
    n.pds = n.pes = n.pfs = n.pgs = 0;
  n.ss = HDATA_SEL;
  n.esp = esp;
  pm_push(&n, pm_flags(flags), isize());
  pm_push(&n, HCODE_SEL, isize());
  pm_push(&n, HBP_IRET, isize());
  n.cs = h.sel;
  n.eip = h.off;
  n.eflags = pm_flags(flags) & ~(EFL_IF | EFL_TF);
  sanitize_sregs(&n);
  enter_frame(&n);
}

/* ---------------------------------------------------------------- */
/* client lifetime                                                  */

static void free_blocks(void)
{
  uint32_t lin, p;
  int i;

  for (i = 0; i < MAX_BLOCKS; i++)
  {
    struct block *b = &cl.blocks[i];
    for (lin = b->lin; b->size && lin < b->lin + b->size; lin += 4096)
      if ((p = unmap_page(lin)) != 0)
        page_free(p);
    b->lin = b->size = 0;
  }
  for (lin = PHYSMAP_BASE; lin < cl.next_phys; lin += 4096)
    unmap_page(lin);
}

/* terminate the client: release everything, then INT 21h AH=4Ch in RM */
static void __attribute__((noreturn)) client_exit(uint8_t code)
{
  struct frame v;
  int i;

  kprintf("dpmi: client exit, code %u\n", code);
  if (cl.env_sel)
    *(uint16_t *)rm_ptr(cl.psp, 0x2c) = cl.env_seg;
  free_blocks();
  for (i = 1; i < LDT_ENTRIES; i++)
    if (ldt_used[i])
      ldt_free(i);
  memset(&v, 0, sizeof(v));
  v.ss = cl.rm_ss;
  v.esp = cl.rm_sp;
  v.ds = v.es = cl.rm_ds;
  v.cs = boot.stub_seg;
  v.eip = boot.stub_bp + BP_RM_RETURN;
  v.eflags = EFL_VM | EFL_IF | 0x2;
  v.eax = 0x4c00 | code;
  cl.active = 0;
  tsp = 0;
  hstack_cur = HOST_STACK;
  vm1.vif = 1;
  v86_reflect(&v, 0x21);
  enter_frame(&v);
}

static void __attribute__((noreturn)) client_fault(struct frame *f, const char *why)
{
  kprintf("dpmi: %s: exception %u, error %04x at %04x:%08x (ss:esp %04x:%08x)\n",
          why, f->vector, f->err, f->cs, f->eip, f->ss, f->esp);
  kprintf("dpmi:   cr2 %08x eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x\n",
          read_cr2(), f->eax, f->ebx, f->ecx, f->edx, f->esi, f->edi);
  kprintf("dpmi:   ds %04x (base %08x) es %04x cs base %08x\n",
          f->pds, sel_base(f->pds), f->pes, sel_base(f->cs));
  client_exit(0xff);
}

/* INT 2Fh AX=1687h in V86: report the host */
void dpmi_detect(struct frame *v)
{
  AX(v) = 0;
  BX(v) = 1;                            /* 32 bit programs supported */
  *(uint8_t *)&v->ecx = 5;              /* Pentium */
  DX(v) = 0x005a;                       /* DPMI 0.90 */
  SI(v) = 0x80;                         /* 2K private data: host RM stack */
  v->es = boot.stub_seg;
  DI(v) = boot.stub_entry;
}

/* V86 breakpoint: initial switch to protected mode */
static void client_enter(struct frame *v)
{
  uint16_t ret_ip = v86_pop16(v), ret_cs = v86_pop16(v);
  struct frame p;
  int i;

  if (cl.active)
  {
    v->eip = ret_ip;
    v->cs = ret_cs;
    AX(v) = ERR_UNSUPPORTED;
    v->eflags |= EFL_CF;
    return;
  }
  memset(&cl, 0, sizeof(cl));
  tsp = 0;
  hstack_cur = HOST_STACK;
  cl.active = 1;
  cl.bits32 = AX(v) & 1;
  cl.psp = *(uint16_t *)rm_ptr(boot.stub_seg, boot.stub_psp);
  cl.rm_ss = (uint16_t)v->es;           /* private data area */
  cl.rm_sp = 0x800;
  cl.rm_ds = (uint16_t)v->ds;
  rm_ss_cur = cl.rm_ss;
  rm_sp_cur = cl.rm_sp;
  cl.next_lin = CLIENT_BASE;
  cl.next_phys = PHYSMAP_BASE;

  i = ldt_alloc(5);
  desc_init(i, (uint32_t)ret_cs << 4, 0xffff, 0xfa, 0x00);      /* CS */
  desc_init(i + 1, v->ds << 4, 0xffff, 0xf2, 0x00);             /* DS */
  desc_init(i + 2, v->ss << 4, 0xffff, 0xf2, 0x00);             /* SS */
  desc_init(i + 3, (uint32_t)cl.psp << 4, 0xff, 0xf2, 0x00);    /* PSP */
  cl.env_seg = *(uint16_t *)rm_ptr(cl.psp, 0x2c);
  if (cl.env_seg)
  {
    uint16_t paras = *(uint16_t *)rm_ptr(cl.env_seg - 1, 3);
    desc_init(i + 4, (uint32_t)cl.env_seg << 4, paras * 16u - 1, 0xf2, 0x00);
    cl.env_sel = LSEL(i + 4);
    *(uint16_t *)rm_ptr(cl.psp, 0x2c) = cl.env_sel;
  }
  else
    ldt_free(i + 4);

  memcpy(&p, v, sizeof(p));
  p.cs = LSEL(i);
  p.eip = ret_ip;
  p.ss = LSEL(i + 2);
  p.esp = v->esp & 0xffff;
  p.pds = LSEL(i + 1);
  p.pes = LSEL(i + 3);
  p.pfs = p.pgs = 0;
  p.eflags = pm_flags(v86_vflags(v)) & ~EFL_CF;
  kprintf("dpmi: %u bit client, PSP %04x\n", cl.bits32 ? 32 : 16, cl.psp);
  enter_frame(&p);
}

/* ---------------------------------------------------------------- */
/* memory                                                           */

static struct block *block_find(uint32_t handle)
{
  int i;

  for (i = 0; i < MAX_BLOCKS; i++)
    if (cl.blocks[i].size && cl.blocks[i].lin == handle)
      return &cl.blocks[i];
  return NULL;
}

/* map `size' bytes of fresh pages at lin; 0 on failure (nothing mapped) */
static int map_fresh(uint32_t lin, uint32_t size)
{
  uint32_t off, p;

  if (size / 4096 + 8 > pages_free())
    return 0;
  for (off = 0; off < size; off += 4096)
  {
    p = page_alloc();
    if (!p || !map_page(lin + off, p, 7))
    {
      if (p)
        page_free(p);
      while (off)
      {
        off -= 4096;
        page_free(unmap_page(lin + off));
      }
      return 0;
    }
  }
  return 1;
}

static void api_alloc(struct frame *f, uint32_t size)
{
  struct block *b;
  int i;

  for (i = 0, b = NULL; i < MAX_BLOCKS && !b; i++)
    if (!cl.blocks[i].size)
      b = &cl.blocks[i];
  size = (size + 4095) & ~4095u;
  if (!size)
    return api_error(f, ERR_VALUE);
  if (!b)
    return api_error(f, ERR_HANDLE);
  if (cl.next_lin + size > CLIENT_END)
    return api_error(f, ERR_LINEAR);
  if (!map_fresh(cl.next_lin, size))
    return api_error(f, ERR_PHYSICAL);
  b->lin = cl.next_lin;
  b->size = size;
  cl.next_lin += size + 4096;           /* guard page */
  BX(f) = b->lin >> 16;
  CX(f) = b->lin & 0xffff;
  SI(f) = b->lin >> 16;
  DI(f) = b->lin & 0xffff;
  set_cf(f, 0);
}

static void api_free(struct frame *f, uint32_t handle)
{
  struct block *b = block_find(handle);
  uint32_t lin;

  if (!b)
    return api_error(f, ERR_HANDLE);
  for (lin = b->lin; lin < b->lin + b->size; lin += 4096)
    page_free(unmap_page(lin));
  b->lin = b->size = 0;
  set_cf(f, 0);
}

static void api_resize(struct frame *f, uint32_t handle, uint32_t size)
{
  struct block *b = block_find(handle);
  uint32_t off, nlin;

  size = (size + 4095) & ~4095u;
  if (!b)
    return api_error(f, ERR_HANDLE);
  if (!size)
    return api_error(f, ERR_VALUE);
  if (size <= b->size)
  {
    for (off = size; off < b->size; off += 4096)
      page_free(unmap_page(b->lin + off));
    b->size = size;
  }
  else if (b->lin + b->size + 4096 == cl.next_lin)
  {
    /* last block: grow in place */
    if (b->lin + size > CLIENT_END)
      return api_error(f, ERR_LINEAR);
    if (!map_fresh(b->lin + b->size, size - b->size))
      return api_error(f, ERR_PHYSICAL);
    b->size = size;
    cl.next_lin = b->lin + size + 4096;
  }
  else
  {
    /* move the pages to a new linear range, then add more */
    nlin = cl.next_lin;
    if (nlin + size > CLIENT_END)
      return api_error(f, ERR_LINEAR);
    if (!map_fresh(nlin + b->size, size - b->size))
      return api_error(f, ERR_PHYSICAL);
    for (off = 0; off < b->size; off += 4096)
      map_page(nlin + off, unmap_page(b->lin + off), 7);
    b->lin = nlin;
    b->size = size;
    cl.next_lin = nlin + size + 4096;
  }
  BX(f) = b->lin >> 16;
  CX(f) = b->lin & 0xffff;
  SI(f) = b->lin >> 16;
  DI(f) = b->lin & 0xffff;
  set_cf(f, 0);
}

static void api_meminfo(struct frame *f)
{
  uint32_t *m = pm_ptr(f->pes, off32(f->edi));
  uint32_t freep = pages_free() > 8 ? pages_free() - 8 : 0;
  int i;

  for (i = 0; i < 12; i++)
    m[i] = 0xffffffffu;
  m[0] = freep * 4096;                  /* largest free block */
  m[1] = freep;                         /* max unlocked allocation */
  m[2] = freep;                         /* max locked allocation */
  m[3] = (CLIENT_END - CLIENT_BASE) / 4096;
  m[4] = pages_total();
  m[5] = freep;
  m[6] = pages_total();
  m[7] = (CLIENT_END - cl.next_lin) / 4096;
  m[8] = 0;                             /* no paging file */
  set_cf(f, 0);
}

static void api_physmap(struct frame *f)
{
  uint32_t phys = (uint32_t)BX(f) << 16 | CX(f);
  uint32_t size = (uint32_t)SI(f) << 16 | DI(f);
  uint32_t lin, off, first = phys & ~0xfffu;

  if (phys + size <= 0x110000)
    lin = phys;                         /* below 1MB: identity */
  else
  {
    size = ((phys & 0xfff) + size + 4095) & ~4095u;
    if (!size || cl.next_phys + size > PHYSMAP_END)
      return api_error(f, ERR_LINEAR);
    for (off = 0; off < size; off += 4096)
      if (!map_page(cl.next_phys + off, first + off, 0x17))     /* PCD */
        return api_error(f, ERR_PHYSICAL);
    lin = cl.next_phys + (phys & 0xfff);
    cl.next_phys += size + 4096;
  }
  BX(f) = lin >> 16;
  CX(f) = lin & 0xffff;
  set_cf(f, 0);
}

/* ---------------------------------------------------------------- */
/* real mode calls                                                  */

/* DPMI 0300h/0301h/0302h */
static void api_rmcall(struct frame *f, int fn)
{
  struct rmregs *r = pm_ptr(f->pes, off32(f->edi));
  struct frame v;
  uint16_t words = CX(f);

  memset(&v, 0, sizeof(v));
  v.eax = r->eax;
  v.ebx = r->ebx;
  v.ecx = r->ecx;
  v.edx = r->edx;
  v.esi = r->esi;
  v.edi = r->edi;
  v.ebp = r->ebp;
  v.ds = r->ds;
  v.es = r->es;
  v.fs = r->fs;
  v.gs = r->gs;
  if (r->ss || r->sp)
  {
    v.ss = r->ss;
    v.esp = r->sp;
  }
  else
  {
    v.ss = rm_ss_cur;
    v.esp = rm_sp_cur;
  }
  v.eflags = EFL_VM | EFL_IF | 0x2 | (r->flags & EFL_USER & ~EFL_TF);

  if (words)                            /* copy parameters from the PM stack */
  {
    const uint8_t *src = (const uint8_t *)sel_base(f->ss) +
        (sel_big(f->ss) ? f->esp : (f->esp & 0xffff));
    v.esp = (uint16_t)(v.esp - words * 2);
    memcpy(rm_ptr(v.ss, v.esp), src, words * 2u);
  }

  if (fn == 0x300)
    rm_int(&v, BL(f), r->flags);
  else
  {
    if (fn == 0x302)
      v86_push16(&v, r->flags | EFL_IF);
    v86_push16(&v, boot.stub_seg);
    v86_push16(&v, boot.stub_bp + BP_RM_RETURN);
    v.cs = r->cs;
    v.eip = r->ip;
    vm1.vif = fn == 0x301 ? (r->flags & EFL_IF) != 0 : 0;
  }
  rm_go(&v, f, T_PM_CALL, (uint32_t)r, 0, 0);
}

/* DPMI 0100h-0102h: DOS memory through INT 21h in real mode */
static void api_dosmem(struct frame *f, int fn)
{
  struct frame v;
  uint32_t sel = DX(f);

  rm_setup(&v, f);
  if (fn == 0x100)
    AH(&v) = 0x48;
  else
  {
    if (!sel_client(sel))
      return api_error(f, ERR_SELECTOR);
    v.es = sel_base(sel) >> 4;
    AH(&v) = fn == 0x101 ? 0x49 : 0x4a;
  }
  rm_int(&v, 0x21, v.eflags);
  rm_go(&v, f, T_PM_DOSMEM, fn, sel, BX(f));
}

static void dosmem_done(struct frame *pm, struct trans *t, struct frame *v)
{
  int fn = t->a, i;
  uint16_t paras = (uint16_t)t->c;

  if (v->eflags & EFL_CF)
  {
    AX(pm) = AX(v);
    BX(pm) = BX(v);
    set_cf(pm, 1);
    return;
  }
  set_cf(pm, 0);
  switch (fn)
  {
    case 0x100:
      i = ldt_alloc(1);
      if (i < 0)
      {
        AX(pm) = 8;
        BX(pm) = 0;
        set_cf(pm, 1);
        return;
      }
      desc_init(i, (uint32_t)AX(v) << 4, paras * 16u - 1, 0xf2, cl.bits32 ? 0x40 : 0);
      AX(pm) = AX(v);
      DX(pm) = LSEL(i);
      break;
    case 0x101:
      ldt_free(t->b >> 3);
      break;
    case 0x102:
      desc_set_limit(&ldt[t->b >> 3], paras * 16u - 1);
      break;
  }
}

/* V86 breakpoint BP_RM_RETURN: a host initiated real mode call is done */
static void rm_return(struct frame *v)
{
  struct trans *t = trans_pop();
  struct frame pm;
  uint32_t fl;

  memcpy(&pm, &t->f, sizeof(pm));
  switch (t->kind)
  {
    case T_PM_INT:
    case T_PM_DEFINT:
      pm.eax = v->eax;
      pm.ebx = v->ebx;
      pm.ecx = v->ecx;
      pm.edx = v->edx;
      pm.esi = v->esi;
      pm.edi = v->edi;
      pm.ebp = v->ebp;
      pm.eflags = (pm.eflags & ~EFL_USER) | (v->eflags & EFL_USER & ~EFL_TF);
      if (t->kind == T_PM_DEFINT)
      {
        /* finish the IRET of the chained handler, with the result flags */
        pm.eip = pm_pop(&pm, isize());
        pm.cs = pm_pop(&pm, isize()) & 0xffff;
        fl = pm_pop(&pm, isize());
        if (!cl.bits32)
          fl = (pm.eflags & 0xffff0000u) | (fl & 0xffff);
        pm.eflags = pm_flags((fl & ~EFL_USER) | (v->eflags & EFL_USER & ~EFL_TF));
      }
      break;
    case T_PM_IRQ:
      break;
    case T_PM_CALL:
    {
      struct rmregs *r = (struct rmregs *)t->a;
      r->eax = v->eax;
      r->ebx = v->ebx;
      r->ecx = v->ecx;
      r->edx = v->edx;
      r->esi = v->esi;
      r->edi = v->edi;
      r->ebp = v->ebp;
      r->flags = (uint16_t)v86_vflags(v);
      r->ds = v->ds;
      r->es = v->es;
      r->fs = v->fs;
      r->gs = v->gs;
      set_cf(&pm, 0);
      break;
    }
    case T_PM_DOSMEM:
      dosmem_done(&pm, t, v);
      break;
    default:
      panic("dpmi: bad real mode return (kind %d)", t->kind);
  }
  vm1.vif = 1;
  sanitize_sregs(&pm);
  dpmi_take_pending(&pm);
  enter_frame(&pm);
}

/* ---------------------------------------------------------------- */
/* real mode callbacks and raw switches                             */

static void rm_callback(struct frame *v, int i)
{
  struct callback *c = &cl.cb[i];
  struct rmregs *r;
  struct frame n;

  if (!c->used)
    panic("dpmi: call to free real mode callback %d", i);
  r = pm_ptr(c->regs.sel, c->regs.off);
  r->eax = v->eax;
  r->ebx = v->ebx;
  r->ecx = v->ecx;
  r->edx = v->edx;
  r->esi = v->esi;
  r->edi = v->edi;
  r->ebp = v->ebp;
  r->flags = (uint16_t)v86_vflags(v);
  r->es = v->es;
  r->ds = v->ds;
  r->fs = v->fs;
  r->gs = v->gs;
  r->cs = v->cs;
  r->ip = (uint16_t)v->eip;
  r->ss = v->ss;
  r->sp = (uint16_t)v->esp;
  desc_set_base(&ldt[c->stack_sel >> 3], v->ss << 4);

  /* the real mode caller's stack is in use below its SP */
  memcpy(&n, v, sizeof(n));
  n.pds = c->stack_sel;
  n.esi = (uint16_t)v->esp;
  n.pes = c->regs.sel;
  n.edi = c->regs.off;
  n.pfs = n.pgs = 0;
  n.eflags = pm_flags(v86_vflags(v));   /* a PM context: keep DS/ES above */
  rm_ss_cur = (uint16_t)v->ss;
  rm_sp_cur = (uint16_t)(v->esp - 0x40);
  pm_handler(&n, c->proc, T_CALLBACK, i, v86_vflags(v));
}

static void callback_done(struct trans *t)
{
  struct callback *c = &cl.cb[t->a];
  struct rmregs *r = pm_ptr(c->regs.sel, c->regs.off);
  struct frame v;

  memset(&v, 0, sizeof(v));
  v.eax = r->eax;
  v.ebx = r->ebx;
  v.ecx = r->ecx;
  v.edx = r->edx;
  v.esi = r->esi;
  v.edi = r->edi;
  v.ebp = r->ebp;
  v.es = r->es;
  v.ds = r->ds;
  v.fs = r->fs;
  v.gs = r->gs;
  v.cs = r->cs;
  v.eip = r->ip;
  v.ss = r->ss;
  v.esp = r->sp;
  v.eflags = EFL_VM | EFL_IF | 0x2 | (r->flags & EFL_USER);
  vm1.vif = (r->flags & EFL_IF) != 0;
  enter_frame(&v);
}

/* V86 breakpoint BP_RAW_TO_PM */
static void raw_to_pm(struct frame *v)
{
  struct frame p;

  memcpy(&p, v, sizeof(p));
  p.pds = AX(v);
  p.pes = CX(v);
  p.ss = DX(v);
  p.esp = off32(v->ebx);
  p.cs = SI(v);
  p.eip = off32(v->edi);
  p.pfs = p.pgs = 0;
  p.eflags = pm_flags(v86_vflags(v));
  sanitize_sregs(&p);
  enter_frame(&p);
}

/* PM breakpoint HBP_RAW */
static void raw_to_rm(struct frame *f)
{
  struct frame v;

  memcpy(&v, f, sizeof(v));
  v.ds = AX(f);
  v.es = CX(f);
  v.ss = DX(f);
  v.esp = BX(f);
  v.cs = SI(f);
  v.eip = DI(f);
  v.fs = v.gs = 0;
  v.eflags = EFL_VM | EFL_IF | 0x2 | (f->eflags & EFL_USER);
  vm1.vif = (f->eflags & EFL_IF) != 0;
  enter_frame(&v);
}

/* V86 breakpoints */
void dpmi_v86_bp(struct frame *v, int bp)
{
  if (bp == BP_DPMI_ENTRY)
    return client_enter(v);
  if (!cl.active)
    panic_frame(v, "DPMI breakpoint without client");
  if (bp == BP_RM_RETURN)
    rm_return(v);
  else if (bp == BP_RAW_TO_PM)
    raw_to_pm(v);
  else if (bp >= BP_CALLBACK && bp < BP_CALLBACK + MAX_CALLBACKS)
    rm_callback(v, bp - BP_CALLBACK);
  panic_frame(v, "bad DPMI breakpoint");
}

/* ---------------------------------------------------------------- */
/* interrupts and exceptions from the client                        */

/* real mode INT 21h AH=4Ch while a client is active (e.g. via 0300h) */
void dpmi_rm_exit(void)
{
  if (!cl.active || !tsp)
    return;
  kprintf("dpmi: client terminated from real mode\n");
  if (cl.env_sel)
    *(uint16_t *)rm_ptr(cl.psp, 0x2c) = cl.env_seg;
  free_blocks();
  memset(ldt_used, 0, sizeof(ldt_used));
  cl.active = 0;
  tsp = 0;
  hstack_cur = HOST_STACK;
}

/* IRQs the CPU accepted while the monitor waited (or V86 had IF off) are
   delivered when we resume the client. Does not return if one is pending. */
int dpmi_take_pending(struct frame *pm)
{
  int irq;

  for (irq = 0; irq < 16; irq++)
    if (vm1.pending & (1u << irq))
    {
      vm1.pending &= ~(1u << irq);
      dpmi_irq(pm, irq);
    }
  return 0;
}

/* hardware IRQ while in V86: run the client's PM handler if it has one.
   Returns 0 if the IRQ should go to real mode as usual. */
int dpmi_irq_v86(struct frame *v, int irq)
{
  uint8_t vec = irq < 8 ? 0x08 + irq : 0x70 + irq - 8;

  if (!cl.active || !cl.pmint[vec].sel)
    return 0;
  rm_ss_cur = (uint16_t)v->ss;
  rm_sp_cur = (uint16_t)(v->esp - 0x40);
  pm_handler(v, cl.pmint[vec], T_TO_PM_HANDLER, 0, v86_vflags(v));
  return 1;
}

/* hardware IRQ while the client runs */
void dpmi_irq(struct frame *f, int irq)
{
  uint8_t vec = irq < 8 ? 0x08 + irq : 0x70 + irq - 8;
  struct frame v;

  if (cl.pmint[vec].sel)
    pm_handler(f, cl.pmint[vec], T_TO_PM_HANDLER, 0, f->eflags);
  rm_setup(&v, f);
  rm_int(&v, vec, f->eflags);
  rm_go(&v, f, T_PM_IRQ, 0, 0, 0);
}

static void pm_int(struct frame *f, uint8_t vec);

/* PM breakpoints in the host code page */
static void host_bp(struct frame *f, uint32_t off)
{
  struct trans *t;

  if (off == HBP_IRET)
  {
    t = trans_pop();
    if (t->kind == T_CALLBACK)
      callback_done(t);
    if (t->kind != T_TO_PM_HANDLER)
      panic("dpmi: bad handler return (kind %d)", t->kind);
    if (!(t->f.eflags & EFL_VM))
      sanitize_sregs(&t->f);
    enter_frame(&t->f);
  }
  if (off == HBP_EXC_RET)
  {
    /* stack: error code, eip, cs, eflags, esp, ss (as left by handler) */
    struct frame r;
    memcpy(&r, f, sizeof(r));
    pm_pop(&r, isize());                /* error code */
    r.eip = pm_pop(&r, isize());
    r.cs = pm_pop(&r, isize()) & 0xffff;
    r.eflags = pm_flags(pm_pop(&r, isize()));
    {
      uint32_t esp = pm_pop(&r, isize());
      r.ss = pm_pop(&r, isize()) & 0xffff;
      r.esp = esp;
    }
    sanitize_sregs(&r);
#ifdef DPMI_TRACE
    kprintf("dpmi: exception handler returns to %04x:%08x ss:esp %04x:%08x\n", r.cs, r.eip, r.ss, r.esp);
#endif
    enter_frame(&r);
  }
  if (off == HBP_RAW)
    raw_to_rm(f);
  if (off >= HBP_DEFINT && off < HBP_DEFINT + 256)
  {
    /* client chained to the default handler: reflect, then IRET */
    struct frame v;
    rm_setup(&v, f);
    rm_int(&v, (uint8_t)(off - HBP_DEFINT), f->eflags);
    rm_go(&v, f, T_PM_DEFINT, 0, 0, 0);
  }
  if (off >= HBP_DEFEXC && off < HBP_DEFEXC + 32)
  {
    f->vector = off - HBP_DEFEXC;
    client_fault(f, "unhandled exception (default handler)");
  }
  client_fault(f, "jump into host code");
}

static void dpmi_api(struct frame *f);

/* software INT n executed by the client */
static void pm_int(struct frame *f, uint8_t vec)
{
  struct frame v;

  if (vec == 0x31)
    return dpmi_api(f);
  if (vec == 0x21 && AH(f) == 0x4c)
    client_exit(AL(f));
  if (cl.pmint[vec].sel)
  {
    pm_push(f, f->eflags, isize());
    pm_push(f, f->cs, isize());
    pm_push(f, f->eip, isize());
    f->eflags &= ~(EFL_IF | EFL_TF);
    f->cs = cl.pmint[vec].sel;
    f->eip = cl.pmint[vec].off;
    return;
  }
  rm_setup(&v, f);
  rm_int(&v, vec, f->eflags);
  rm_go(&v, f, T_PM_INT, 0, 0, 0);
}

static void pm_exception(struct frame *f)
{
  unsigned n = f->vector;
  struct frame e;
  uint32_t esp;

  if (n >= 32 || !cl.exc[n].sel)
    client_fault(f, "unhandled exception");
  esp = (f->ss == HDATA_SEL && f->esp - 0x100 < hstack_cur) ? (f->esp - 0x100) & ~3u : hstack_cur;
  memcpy(&e, f, sizeof(e));
  e.ss = HDATA_SEL;
  e.esp = esp;
  pm_push(&e, f->ss, isize());
  pm_push(&e, f->esp, isize());
  pm_push(&e, f->eflags, isize());
  pm_push(&e, f->cs, isize());
  pm_push(&e, f->eip, isize());
  pm_push(&e, f->err, isize());
  pm_push(&e, HCODE_SEL, isize());
  pm_push(&e, HBP_EXC_RET, isize());
  e.cs = cl.exc[n].sel;
  e.eip = cl.exc[n].off;
  e.eflags &= ~EFL_TF;
#ifdef DPMI_TRACE
  kprintf("dpmi: exception %u at %04x:%08x err %x -> %04x:%08x\n", n, f->cs, f->eip, f->err, e.cs, e.eip);
#endif
  enter_frame(&e);
}

/* traps from the client (ring 3, not V86) */
void dpmi_trap(struct frame *f)
{
  /* INT n through a DPL 0 gate faults with error (n << 3) | 2 (IDT bit);
     HLT and other privileged instructions with error 0 */
  if (f->vector == 0x0d && (f->err == 0 || (f->err & 7) == 2))
  {
    const uint8_t *code = (const uint8_t *)(sel_base(f->cs) + f->eip);
    int len = 0;

    while (len < 8 && (code[len] == 0x66 || code[len] == 0x67 || code[len] == 0x26 ||
                       code[len] == 0x2e || code[len] == 0x36 || code[len] == 0x3e ||
                       code[len] == 0x64 || code[len] == 0x65))
      len++;
    if (code[len] == 0xf4 && (f->cs & ~3u) == (HCODE_SEL & ~3u))
      return host_bp(f, f->eip);
    if (code[len] == 0xcd)
    {
      uint8_t vec = code[len + 1];
      f->eip += len + 2;
      if (!sel_big(f->cs))
        f->eip &= 0xffff;
      return pm_int(f, vec);
    }
    if (code[len] == 0xcc)
    {
      f->eip += len + 1;
      return pm_int(f, 3);
    }
    if (code[len] == 0xf4)              /* HLT: wait for an interrupt */
    {
      f->eip += len + 1;
      while (!vm1.pending)
        __asm__ volatile("sti; hlt; cli" ::: "memory");
      dpmi_take_pending(f);
      return;
    }
  }
  if (f->vector >= 0x20 && f->vector < 0x30)
    return dpmi_irq(f, (int)f->vector - 0x20);
  pm_exception(f);
}

/* ---------------------------------------------------------------- */
/* INT 31h                                                          */

static void api_descriptor(struct frame *f)
{
  uint16_t sel = BX(f);
  int i = sel >> 3, n;

  if (AX(f) != 0x0000 && AX(f) != 0x0002 && AX(f) != 0x0003 && !sel_client(sel))
    return api_error(f, ERR_SELECTOR);
  switch (AX(f))
  {
    case 0x0000:                        /* allocate CX descriptors */
      n = ldt_alloc(CX(f) ? CX(f) : 1);
      if (n < 0)
        return api_error(f, ERR_DESC);
      AX(f) = LSEL(n);
      break;
    case 0x0001:                        /* free descriptor */
      ldt_free(i);
      if (f->pds == sel) f->pds = 0;
      if (f->pes == sel) f->pes = 0;
      if (f->pfs == sel) f->pfs = 0;
      if (f->pgs == sel) f->pgs = 0;
      break;
    case 0x0002:                        /* segment to descriptor */
    {
      uint32_t base = (uint32_t)BX(f) << 4;
      for (n = 1; n < LDT_ENTRIES; n++)
        if (ldt_used[n] == 2 && desc_base(&ldt[n]) == base)
          break;
      if (n == LDT_ENTRIES)
      {
        n = ldt_alloc(1);
        if (n < 0)
          return api_error(f, ERR_DESC);
        desc_init(n, base, 0xffff, 0xf2, 0x00);
        ldt_used[n] = 2;                /* not freeable by convention */
      }
      AX(f) = LSEL(n);
      break;
    }
    case 0x0003:                        /* selector increment */
      AX(f) = 8;
      break;
    case 0x0006:                        /* get base */
    {
      uint32_t b = desc_base(&ldt[i]);
      CX(f) = b >> 16;
      DX(f) = b & 0xffff;
      break;
    }
    case 0x0007:                        /* set base */
      desc_set_base(&ldt[i], (uint32_t)CX(f) << 16 | DX(f));
      break;
    case 0x0008:                        /* set limit */
    {
      uint32_t l = (uint32_t)CX(f) << 16 | DX(f);
      if (l > 0xfffff && (l & 0xfff) != 0xfff)
        return api_error(f, ERR_VALUE);
      desc_set_limit(&ldt[i], l);
      break;
    }
    case 0x0009:                        /* set access rights */
      if ((CL(f) & 0x70) != 0x70)       /* must be DPL 3, code/data */
        return api_error(f, ERR_VALUE);
      ldt[i].access = CL(f) | 0x01;
      ldt[i].gran = (ldt[i].gran & 0x0f) | (CH(f) & 0xd0);
      break;
    case 0x000a:                        /* data alias */
      n = ldt_alloc(1);
      if (n < 0)
        return api_error(f, ERR_DESC);
      ldt[n] = ldt[i];
      ldt[n].access = (ldt[n].access & 0xf0) | 0x02;
      AX(f) = LSEL(n);
      break;
    case 0x000b:                        /* get descriptor */
      memcpy(pm_ptr(f->pes, off32(f->edi)), &ldt[i], 8);
      break;
    case 0x000c:                        /* set descriptor */
    {
      struct desc d;
      memcpy(&d, pm_ptr(f->pes, off32(f->edi)), 8);
      if ((d.access & 0x70) != 0x70)
        return api_error(f, ERR_VALUE);
      ldt[i] = d;
      break;
    }
    default:
      return api_error(f, ERR_UNSUPPORTED);
  }
  set_cf(f, 0);
}

static void set_farptr(struct frame *f, struct farptr p)
{
  CX(f) = p.sel;
  if (cl.bits32)
    f->edx = p.off;
  else
    DX(f) = (uint16_t)p.off;
}

static struct farptr get_farptr(struct frame *f)
{
  struct farptr p;
  p.sel = CX(f);
  p.off = off32(f->edx);
  return p;
}

static void dpmi_api(struct frame *f)
{
  uint8_t n = BL(f);
  struct farptr p;
  int i;

#ifdef DPMI_TRACE
  kprintf("int31 %04x bx=%04x cx=%04x dx=%04x si=%04x di=%08x ss:esp=%04x:%08x cs:eip=%04x:%08x\n",
          AX(f), BX(f), CX(f), DX(f), SI(f), f->edi, f->ss, f->esp, f->cs, f->eip);
#endif
  if (AH(f) == 0x00)
    return api_descriptor(f);
  set_cf(f, 0);
  switch (AX(f))
  {
    case 0x0100:
    case 0x0101:
    case 0x0102:
      return api_dosmem(f, AX(f));
    case 0x0200:                        /* get real mode vector */
    {
      uint16_t *vec = rm_ptr(0, n * 4u);
      DX(f) = vec[0];
      CX(f) = vec[1];
      return;
    }
    case 0x0201:                        /* set real mode vector */
    {
      uint16_t *vec = rm_ptr(0, n * 4u);
      vec[0] = DX(f);
      vec[1] = CX(f);
      return;
    }
    case 0x0202:                        /* get exception handler */
      if (n >= 32)
        return api_error(f, ERR_VALUE);
      p = cl.exc[n];
      if (!p.sel)
      {
        p.sel = HCODE_SEL;
        p.off = HBP_DEFEXC + n;
      }
      return set_farptr(f, p);
    case 0x0203:                        /* set exception handler */
      if (n >= 32)
        return api_error(f, ERR_VALUE);
      p = get_farptr(f);
      cl.exc[n] = p.sel == HCODE_SEL ? (struct farptr){ 0, 0 } : p;
      return;
    case 0x0204:                        /* get PM interrupt vector */
      p = cl.pmint[n];
      if (!p.sel)
      {
        p.sel = HCODE_SEL;
        p.off = HBP_DEFINT + n;
      }
      return set_farptr(f, p);
    case 0x0205:                        /* set PM interrupt vector */
      p = get_farptr(f);
      cl.pmint[n] = p.sel == HCODE_SEL ? (struct farptr){ 0, 0 } : p;
      return;
    case 0x0300:
    case 0x0301:
    case 0x0302:
      return api_rmcall(f, AX(f));
    case 0x0303:                        /* allocate real mode callback */
      for (i = 0; i < MAX_CALLBACKS && cl.cb[i].used; i++)
        ;
      if (i == MAX_CALLBACKS)
        return api_error(f, ERR_CALLBACK);
      {
        int s = ldt_alloc(1);
        if (s < 0)
          return api_error(f, ERR_DESC);
        desc_init(s, 0, 0xffff, 0xf2, 0x00);
        cl.cb[i].stack_sel = LSEL(s);
      }
      cl.cb[i].used = 1;
      cl.cb[i].proc.sel = (uint16_t)f->pds;
      cl.cb[i].proc.off = off32(f->esi);
      cl.cb[i].regs.sel = (uint16_t)f->pes;
      cl.cb[i].regs.off = off32(f->edi);
      CX(f) = boot.stub_seg;
      DX(f) = boot.stub_bp + BP_CALLBACK + i;
      return;
    case 0x0304:                        /* free real mode callback */
      i = DX(f) - boot.stub_bp - BP_CALLBACK;
      if (CX(f) != boot.stub_seg || i < 0 || i >= MAX_CALLBACKS || !cl.cb[i].used)
        return api_error(f, ERR_CALLBACK);
      cl.cb[i].used = 0;
      ldt_free(cl.cb[i].stack_sel >> 3);
      return;
    case 0x0305:                        /* state save/restore addresses */
      AX(f) = 0;
      BX(f) = boot.stub_seg;
      CX(f) = boot.stub_retf;
      SI(f) = HCODE_SEL;
      if (cl.bits32)
        f->edi = HBP_RETF;
      else
        DI(f) = HBP_RETF;
      return;
    case 0x0306:                        /* raw mode switch addresses */
      BX(f) = boot.stub_seg;
      CX(f) = boot.stub_bp + BP_RAW_TO_PM;
      SI(f) = HCODE_SEL;
      if (cl.bits32)
        f->edi = HBP_RAW;
      else
        DI(f) = HBP_RAW;
      return;
    case 0x0400:                        /* version */
      AX(f) = 0x005a;
      BX(f) = 0x0001;                   /* 32 bit host */
      CL(f) = 5;
      DX(f) = 0x0870;                   /* PIC bases as seen by clients */
      return;
    case 0x0500:
      return api_meminfo(f);
    case 0x0501:
      return api_alloc(f, (uint32_t)BX(f) << 16 | CX(f));
    case 0x0502:
      return api_free(f, (uint32_t)SI(f) << 16 | DI(f));
    case 0x0503:
      return api_resize(f, (uint32_t)SI(f) << 16 | DI(f), (uint32_t)BX(f) << 16 | CX(f));
    case 0x0600: case 0x0601: case 0x0602: case 0x0603:
    case 0x0702: case 0x0703:
      return;                           /* no virtual memory: nothing to do */
    case 0x0604:                        /* page size */
      BX(f) = 0;
      CX(f) = 4096;
      return;
    case 0x0800:
      return api_physmap(f);
    case 0x0801:
      return;
    case 0x0900:                        /* get and disable virtual IF */
      AL(f) = (f->eflags & EFL_IF) != 0;
      f->eflags &= ~EFL_IF;
      return;
    case 0x0901:                        /* get and enable */
      AL(f) = (f->eflags & EFL_IF) != 0;
      f->eflags |= EFL_IF;
      return;
    case 0x0902:                        /* get */
      AL(f) = (f->eflags & EFL_IF) != 0;
      return;
  }
  api_error(f, ERR_UNSUPPORTED);
}

/* ---------------------------------------------------------------- */

void dpmi_init(void)
{
  uint32_t off, p;
  uint8_t *code = (uint8_t *)HOST_AREA;

  /* host code page (read only for clients) and host PM stack */
  p = page_alloc();
  if (!p || !map_page(HOST_AREA, p, 5))
    panic("dpmi: no memory");
  memset(code, 0xf4, 4096);             /* HLT everywhere ... */
  code[HBP_RETF] = 0xcb;                /* ... except the no-op RETF */
  for (off = 4096; off < HOST_STACK; off += 4096)
  {
    p = page_alloc();
    if (!p || !map_page(HOST_AREA + off, p, 7))
      panic("dpmi: no memory");
  }
  gdt_set(HCODE_SEL >> 3, HOST_AREA, 0xfff, 0xfa, 0x40);
  gdt_set(HDATA_SEL >> 3, HOST_AREA, HOST_STACK - 1, 0xf2, 0x40);
  ldt_load(ldt, LDT_ENTRIES);
}
