/* MDVMM core - DOS sessions
 *
 * All sessions share one DOS kernel, as with Windows 386 enhanced mode:
 *  - memory below the loader's resident block is global (shared pages);
 *    from there to 640K every session has private pages
 *  - "instance data" (IVT, BIOS data area, the DOS data segment from the
 *    kernel's 1605h table, the CDS array) is swapped on every switch
 *  - a session is only switched out while it is not inside a DOS critical
 *    section (INT 2Ah AH=80h..82h), so global DOS structures (buffers,
 *    FAT, SFTs) are never seen half updated
 *  - all hardware interrupts go to the running session; keystrokes for a
 *    focused session that is not running are queued and replayed
 *  - text mode video: the focused session owns B8000h, the others write
 *    to shadow pages
 *  - a DPMI client belongs to the session it started in; the others keep
 *    running (as plain V86) around it, switches happen in PM as well
 * Session 0 is the original DOS (identity mapped memory).
 */
#include "core.h"

#define MAX_SESSIONS  4
#define MAX_RANGES    8
#define SLICE_TICKS   2                 /* ~110 ms per time slice */
#define KQ_SIZE       32
#define VRAM          0xB8000u
#define INST_BASE     0xEE000000u       /* instance data copies, 1MB each */

struct range { uint32_t lin, size; };

struct session {
  int used;
  struct frame f;                       /* saved V86 state */
  uint32_t vif, vflags, crit;
  uint32_t pages[160];                  /* private page per 4K below 640K */
  uint32_t vram[VRAM_PAGES];            /* shadow video pages */
  uint32_t inst;                        /* linear address of instance copy */
  uint32_t tick_mark;
  uint8_t kq[KQ_SIZE];                  /* queued scancodes */
  int kq_head, kq_tail;
  int slot;                             /* a job slot (SSHSLOT.COM) */
  int job;                              /* JOB_* */
  uint16_t job_flags, job_code;
  char cmd[128];
};

enum { JOB_NONE, JOB_ASSIGNED, JOB_RUNNING, JOB_DONE };

/* physical pages of the visible text screen: VGA memory, or RAM that
   fbcon.c draws on a UEFI framebuffer */
uint32_t vram_phys[VRAM_PAGES] = {
  0xB8000, 0xB9000, 0xBA000, 0xBB000, 0xBC000, 0xBD000, 0xBE000, 0xBF000
};

static struct session ses[MAX_SESSIONS];

static struct range ranges[MAX_RANGES];
static int nranges;
static uint32_t inst_size;              /* bytes of instance data */
static uint32_t first_private;          /* first private page number */
static int registered;
static int cur, focus, nsessions = 1;
static int want = -1;                   /* session to switch to */
static uint32_t ticks, slice;
static uint32_t crit;                   /* critical section depth */
static int kbd_ctrl, kbd_alt;
static int vm_trace;                    /* log every session switch */
static int latch_valid;                 /* port 60h replay */
static uint8_t latch;


/* ---------------------------------------------------------------- */

static void add_range(uint32_t lin, uint32_t size)
{
  if (!size || nranges == MAX_RANGES)
    return;
  ranges[nranges].lin = lin;
  ranges[nranges].size = size;
  inst_size += size;
  nranges++;
  kprintf("vm: instance data %05x, %u bytes\n", lin, size);
}

static uint32_t far_lin(uint32_t fp)
{
  return ((fp >> 16) << 4) + (fp & 0xffff);
}

/* INT 2Fh AX=DE04h: the loader registers per session data (DS:DX) */
static uint32_t indos_lin;              /* DOS InDOS flag: SDA + 1 */

/* DOS's current PSP (SDA + 10h), or -1 if DOS has not told us where */
int dos_current_psp(void)
{
  if (!indos_lin)
    return -1;
  return *(const uint16_t *)(indos_lin - 1 + 0x10);
}

static int vm_register(struct frame *f)
{
  const uint8_t *r = rm_ptr(f->ds, DX(f));
  uint32_t sis = *(const uint32_t *)r;
  uint32_t cds = *(const uint32_t *)(r + 8);
  uint16_t cds_size = *(const uint16_t *)(r + 12);
  uint16_t boundary = *(const uint16_t *)(r + 14);
  int guard;

  if (!indos_lin && *(const uint32_t *)(r + 4))
    indos_lin = far_lin(*(const uint32_t *)(r + 4));
  if (registered || !sis || (boundary & 0xff))
    return 0;
  add_range(0, 0x600);                  /* IVT, BIOS data, DOS 500h area */
  for (guard = 0; sis && guard < 16; guard++)
  {
    const uint8_t *s = (const uint8_t *)far_lin(sis);
    uint32_t inst = *(const uint32_t *)(s + 0x0e);
    const uint8_t *e;

    for (e = inst ? (const uint8_t *)far_lin(inst) : 0; e && *(const uint32_t *)e; e += 6)
      add_range(far_lin(*(const uint32_t *)e), *(const uint16_t *)(e + 4));
    sis = *(const uint32_t *)(s + 2);
  }
  if (nranges < 2)
  {
    kprintf("vm: kernel reported no instance data\n");
    return 0;
  }
  add_range(far_lin(cds), cds_size);
  first_private = (uint32_t)boundary >> 8;
  ses[0].used = 1;
  registered = 1;
  kprintf("vm: sessions enabled, private memory %05x-9FFFF\n", first_private << 12);
  return 1;
}

/* instance data: live memory <-> a session's saved copy */
static void inst_copy(struct session *s, int save)
{
  uint8_t *buf = (uint8_t *)s->inst;
  int i;

  for (i = 0; i < nranges; i++)
  {
    if (save)
      memcpy(buf, (void *)ranges[i].lin, ranges[i].size);
    else
      memcpy((void *)ranges[i].lin, buf, ranges[i].size);
    buf += ranges[i].size;
  }
}

/* the BIOS data area of a session: live, or in its instance copy */
static uint8_t *bda(int n)
{
  return n == cur ? (uint8_t *)rm_ptr(0x40, 0) : (uint8_t *)ses[n].inst + 0x400;
}

static void map_session(int n)
{
  struct session *s = &ses[n];
  uint32_t p;

  for (p = first_private; p < 0xa0; p++)
    map_page(p << 12, s->pages[p] ? s->pages[p] : p << 12, 7);
  for (p = 0; p < VRAM_PAGES; p++)
    map_page(VRAM + p * 4096, n == focus ? vram_phys[p] : s->vram[p], 7);
}

/* in-service IRQs at the PICs */
static uint16_t pic_isr(void)
{
  uint16_t isr;

  outb(0x20, 0x0b);
  outb(0xa0, 0x0b);
  isr = inb(0x20) | inb(0xa0) << 8;
  outb(0x20, 0x0a);
  outb(0xa0, 0x0a);
  return isr;
}

int vm_current(void)
{
  return cur;
}

/* set while the DPMI host runs a monitor service for its client with a
   V86 frame that is never entered: switches wait for the next chance */
int vm_hold;

static int switchable(const struct frame *f, uint16_t isr_ignore)
{
  int ifl = (f->eflags & EFL_VM) ? (int)vm1.vif : !!(f->eflags & EFL_IF);

  return ifl && !vm_hold && !crit && !(pic_isr() & ~isr_ignore);
}

static int next_session(void)
{
  int i, n;

  for (i = 1; i <= MAX_SESSIONS; i++)
  {
    n = (cur + i) % MAX_SESSIONS;
    if (ses[n].used)
      return n;
  }
  return cur;
}

/* replay a queued keystroke into the running session (virtual IRQ1);
   in protected mode (a DPMI client) through the client's IRQ1 path */
static void kbd_inject(struct frame *f)
{
  struct session *s = &ses[cur];
  int pm = !(f->eflags & EFL_VM);

  if (vm_hold || s->kq_head == s->kq_tail || latch_valid || pic_isr())
    return;
  if (pm ? !(f->eflags & EFL_IF) : !vm1.vif)
    return;
  latch = s->kq[s->kq_head];
  s->kq_head = (s->kq_head + 1) % KQ_SIZE;
  latch_valid = 1;
  if (pm)
    dpmi_irq(f, 1);                     /* does not return */
  v86_reflect(f, 0x09);
}

static struct frame next_frame;

static void __attribute__((noreturn)) switch_to(struct frame *f, int n, int save)
{
  struct session *a = &ses[cur], *b = &ses[n];
  uint32_t missed, *bios_ticks;

  if (save)
  {
    memcpy(&a->f, f, sizeof(*f));
    a->vif = vm1.vif;
    a->vflags = vm1.vflags;
    a->crit = crit;
    a->tick_mark = ticks;
    inst_copy(a, 1);
  }
  if (vm_trace)
    kprintf("vm: %d -> %d (%s)\n", cur, n, (b->f.eflags & EFL_VM) ? "v86" : "pm");
  cur = n;
  map_session(n);
  inst_copy(b, 0);
  vm1.vif = b->vif;
  vm1.vflags = b->vflags;
  crit = b->crit;
  want = -1;
  slice = 0;

  /* timer ticks this session missed while it was not running */
  missed = ticks - b->tick_mark;
  bios_ticks = rm_ptr(0x40, 0x6c);
  if (missed > 1)
  {
    *bios_ticks += missed - 1;
    if (*bios_ticks >= 0x1800b0)
    {
      *bios_ticks -= 0x1800b0;
      *(uint8_t *)rm_ptr(0x40, 0x70) = 1;
    }
  }

  /* not into *f: a trap frame from protected mode is 16 bytes shorter
     than struct frame (no V86 segment registers), at the top of the stack */
  f = &next_frame;
  memcpy(f, &b->f, sizeof(*f));
  if (f->eflags & EFL_VM)
    v86_deliver(f);
  else
    dpmi_take_pending(f);               /* a DPMI client: IRQs in PM */
  kbd_inject(f);
  enter_frame(f);
}

/* called at points where a pending switch may be done (STI, IRET, ...) */
void vm_poll(struct frame *f)
{
  if (want >= 0 && want != cur && ses[want].used && switchable(f, 0))
    switch_to(f, want, 1);
  kbd_inject(f);
}

/* timer interrupt (IRQ0 pending, not yet delivered) */
void vm_tick(struct frame *f)
{
  ticks++;
  if (nsessions < 2)
    return;
  if (want == cur || (want >= 0 && !ses[want].used))
    want = -1;                          /* nothing to switch to */
  if (++slice >= SLICE_TICKS && want < 0)
    want = next_session();
  if (want >= 0 && want != cur && ses[want].used && switchable(f, 1))
    switch_to(f, want, 1);
}

/* the running session gives up the CPU (idle, HLT) */
void vm_yield(struct frame *f)
{
  if (nsessions < 2)
    return;
  if (want < 0)
    want = next_session();
  vm_poll(f);
}

/* ---------------------------------------------------------------- */
/* focus, keyboard                                                  */

static void set_focus(int n)
{
  int old = focus, i;
  uint8_t *b;
  uint16_t pos;

  if (n == focus || n < 0 || n >= MAX_SESSIONS || !ses[n].used)
    return;
  for (i = 0; i < VRAM_PAGES; i++)
  {
    phys_copy(ses[old].vram[i], vram_phys[i], 4096);
    phys_copy(vram_phys[i], ses[n].vram[i], 4096);
  }
  focus = n;
  map_session(cur);

  /* the hotkey's Ctrl/Alt presses went to the old session: release them */
  b = bda(old);
  b[0x17] &= ~0x0c;
  b[0x18] &= ~0x03;
  /* cursor of the new session (page 0) */
  b = bda(n);
  pos = b[0x51] * 80 + b[0x50];
  outb(0x3d4, 0x0e);
  outb(0x3d5, pos >> 8);
  outb(0x3d4, 0x0f);
  outb(0x3d5, pos & 0xff);
  if (n != cur)
    want = n;                           /* show it running soon */
  kprintf("vm: focus session %d\n", n);
}

/* IRQ1: hotkeys and keystrokes for a focused session that is not running.
   Returns 1 if the interrupt was consumed. */
int vm_kbd(struct frame *f)
{
  uint8_t sc;

  (void)f;
  if (!registered)
    return 0;
  sc = inb(0x60);
  latch = sc;
  latch_valid = 1;                      /* the session reads the same byte */
  switch (sc & 0x7f)
  {
    case 0x1d: kbd_ctrl = !(sc & 0x80); break;
    case 0x38: kbd_alt = !(sc & 0x80); break;
  }
  if (kbd_ctrl && kbd_alt && sc >= 0x3b && sc <= 0x3e)    /* F1..F4 */
  {
    latch_valid = 0;
    outb(0x20, 0x61);                   /* EOI IRQ1 ourselves */
    vm1.pending &= ~2u;
    set_focus(sc - 0x3b);
    return 1;
  }
  if (focus != cur && nsessions > 1)
  {
    struct session *s = &ses[focus];
    int next = (s->kq_tail + 1) % KQ_SIZE;
    if (next != s->kq_head)
    {
      s->kq[s->kq_tail] = sc;
      s->kq_tail = next;
    }
    latch_valid = 0;
    outb(0x20, 0x61);
    vm1.pending &= ~2u;
    want = focus;
    return 1;
  }
  return 0;
}

/* a keystroke from a keyboard other than the PS/2 controller (USB):
   hotkeys as on IRQ1, otherwise queued for the focused session, which
   gets it as a virtual IRQ1 with the scancode at port 60h */
void vm_key(uint8_t sc)
{
  struct session *s = &ses[focus];
  int next = (s->kq_tail + 1) % KQ_SIZE;

  switch (sc & 0x7f)
  {
    case 0x1d: kbd_ctrl = !(sc & 0x80); break;
    case 0x38: kbd_alt = !(sc & 0x80); break;
  }
  if (registered && kbd_ctrl && kbd_alt && sc >= 0x3b && sc <= 0x3e)
  {
    set_focus(sc - 0x3b);
    return;
  }
  if (next != s->kq_head)
  {
    s->kq[s->kq_tail] = sc;
    s->kq_tail = next;
  }
  if (focus != cur && nsessions > 1)
    want = focus;
}

/* scancodes waiting for the running session */
int vm_key_pending(void)
{
  return ses[cur].kq_head != ses[cur].kq_tail;
}

/* port 60h read by a session: replay the latched scancode once */
int vm_port60(uint8_t *v)
{
  if (!latch_valid)
    return 0;
  *v = latch;
  latch_valid = 0;
  return 1;
}

/* ---------------------------------------------------------------- */
/* session lifetime                                                 */

static int alloc_session_pages(struct session *s)
{
  uint32_t p, n;

  for (p = first_private; p < 0xa0; p++)
    if (!(s->pages[p] = page_alloc()))
      return 0;
  for (p = 0; p < VRAM_PAGES; p++)
    if (!(s->vram[p] = page_alloc()))
      return 0;
  for (n = 0; n < (inst_size + 4095) / 4096; n++)
  {
    p = page_alloc();
    if (!p || !map_page(INST_BASE + (uint32_t)(s - ses) * 0x100000 + n * 4096, p, 3))
      return 0;
  }
  s->inst = INST_BASE + (uint32_t)(s - ses) * 0x100000;
  return 1;
}

static void free_session_pages(struct session *s)
{
  uint32_t p, n;

  for (p = first_private; p < 0xa0; p++)
    if (s->pages[p])
      page_free(s->pages[p]);
  for (p = 0; p < VRAM_PAGES; p++)
    if (s->vram[p])
      page_free(s->vram[p]);
  for (n = 0; n < (inst_size + 4095) / 4096; n++)
  {
    uint32_t phys = unmap_page(s->inst + n * 4096);
    if (phys)
      page_free(phys);
  }
  memset(s, 0, sizeof(*s));
}

/* INT 2Fh AX=DE02h: fork the running session.
   Parent: AX = new session number; child: AX = 0, BX = its number. */
static void vm_fork(struct frame *f)
{
  struct session *s;
  uint32_t p;
  int n;

  for (n = 0; n < MAX_SESSIONS && ses[n].used; n++)
    ;
  if (!registered || n == MAX_SESSIONS || dpmi_owner() == cur)
    goto fail;
  s = &ses[n];
  memset(s, 0, sizeof(*s));
  if (!alloc_session_pages(s))
  {
    free_session_pages(s);
    goto fail;
  }
  /* private memory and screen: copies of the current session's */
  for (p = first_private; p < 0xa0; p++)
    phys_copy(s->pages[p], ses[cur].pages[p] ? ses[cur].pages[p] : p << 12, 4096);
  for (p = 0; p < VRAM_PAGES; p++)
    memcpy(kmap(2, s->vram[p]), (void *)(VRAM + p * 4096), 4096);
  inst_copy(s, 1);
  memcpy(&s->f, f, sizeof(*f));
  s->f.eflags &= ~EFL_CF;
  *(uint16_t *)&s->f.eax = 0;
  *(uint16_t *)&s->f.ebx = (uint16_t)n;
  s->vif = vm1.vif;
  s->vflags = vm1.vflags;
  s->crit = 0;
  s->tick_mark = ticks;
  s->used = 1;
  nsessions++;
  kprintf("vm: session %d started (%u sessions)\n", n, nsessions);
  AX(f) = (uint16_t)n;
  f->eflags &= ~EFL_CF;
  return;
fail:
  AX(f) = 0xffff;
  f->eflags |= EFL_CF;
}

/* INT 2Fh AX=DE03h: end the running session (not session 0) */
static void vm_end(struct frame *f)
{
  int n = cur;

  if (n == 0)
  {
    f->eflags |= EFL_CF;
    return;
  }
  if (focus == n)
  {
    int i;
    for (i = 0; i < VRAM_PAGES; i++)
      phys_copy(vram_phys[i], ses[0].vram[i], 4096);
    focus = 0;
  }
  free_session_pages(&ses[n]);
  nsessions--;
  kprintf("vm: session %d ended (%u sessions)\n", n, nsessions);
  switch_to(f, 0, 0);
}

/* ---------------------------------------------------------------- */
/* remote consoles: screens, keys and jobs of other sessions        */

/* INT 2Fh AX=DE08h: BL = session, ES:DI buffer, CX its size.
   Copies the visible text page (cells of char + attribute).
   Returns AH:AL = cursor row:column, BX = cursor shape, CL = video mode,
   CH = columns, DL = rows, DH = active page, SI = bytes copied. */
static void api_screen(struct frame *f)
{
  int n = BL(f), cols, rows;
  uint32_t start, len, done = 0, room = CX(f);
  uint8_t *b, *dst;
  struct session *s;

  if (n < 0 || n >= MAX_SESSIONS || !ses[n].used)
  {
    f->eflags |= EFL_CF;
    return;
  }
  s = &ses[n];
  b = bda(n);
  cols = b[0x4a] ? b[0x4a] : 80;
  rows = b[0x84] ? b[0x84] + 1 : 25;
  start = *(uint16_t *)(b + 0x4e);
  len = (uint32_t)cols * rows * 2;
  if (len > room)
    len = room;
  if (start + len > VRAM_PAGES * 4096u)
    len = start < VRAM_PAGES * 4096u ? VRAM_PAGES * 4096u - start : 0;
  dst = rm_ptr((uint16_t)f->es, DI(f));
  while (done < len)
  {
    uint32_t off = start + done, p = off >> 12, chunk = 4096 - (off & 4095);
    uint32_t phys = n == focus ? vram_phys[p] : s->vram[p];
    if (chunk > len - done)
      chunk = len - done;
    memcpy(dst + done, (uint8_t *)kmap(2, phys) + (off & 4095), chunk);
    done += chunk;
  }
  AX(f) = (uint16_t)(b[0x50 + b[0x62] * 2 + 1] << 8 | b[0x50 + b[0x62] * 2]);
  BX(f) = *(uint16_t *)(b + 0x60);
  *(uint8_t *)&f->ecx = b[0x49];
  *((uint8_t *)&f->ecx + 1) = (uint8_t)cols;
  *(uint8_t *)&f->edx = (uint8_t)rows;
  *((uint8_t *)&f->edx + 1) = b[0x62];
  SI(f) = (uint16_t)done;
  f->eflags &= ~EFL_CF;
}

/* INT 2Fh AX=DE09h: BL = session, DS:SI keys (words: scan << 8 | ASCII),
   CX count -> into its BIOS keyboard buffer; CX = keys taken */
static void api_keys(struct frame *f)
{
  int n = BL(f), i;
  uint8_t *b;
  uint16_t *keys, start, end;

  if (n < 0 || n >= MAX_SESSIONS || !ses[n].used)
  {
    f->eflags |= EFL_CF;
    return;
  }
  b = bda(n);
  keys = rm_ptr((uint16_t)f->ds, SI(f));
  start = *(uint16_t *)(b + 0x80);
  end = *(uint16_t *)(b + 0x82);
  if (!start || end <= start)
  {
    start = 0x1e;
    end = 0x3e;
  }
  for (i = 0; i < CX(f); i++)
  {
    uint16_t tail = *(uint16_t *)(b + 0x1c), next = tail + 2;
    if (next >= end)
      next = start;
    if (next == *(uint16_t *)(b + 0x1a))
      break;                            /* full */
    *(uint16_t *)(b + tail) = keys[i];
    *(uint16_t *)(b + 0x1c) = next;
  }
  CX(f) = (uint16_t)i;
  if (i && n != cur)
    want = n;                           /* let it see them soon */
  f->eflags &= ~EFL_CF;
}

/* INT 2Fh AX=DE0Ah (a job slot asks for work): ES:DI 128 byte buffer.
   CF if there is none; else the command (ASCIZ, empty: interactive
   shell) and AX = flags. */
static void api_job_get(struct frame *f)
{
  struct session *s = &ses[cur];

  s->slot = 1;
  if (s->job != JOB_ASSIGNED)
  {
    f->eflags |= EFL_CF;
    return;
  }
  memcpy(rm_ptr((uint16_t)f->es, DI(f)), s->cmd, sizeof(s->cmd));
  s->job = JOB_RUNNING;
  kprintf("vm: session %d runs job \"%s\"\n", cur, s->cmd);
  AX(f) = s->job_flags;
  f->eflags &= ~EFL_CF;
}

/* INT 2Fh AX=DE0Bh (a job slot is done): BX = exit code */
static void api_job_done(struct frame *f)
{
  struct session *s = &ses[cur];

  if (s->job == JOB_RUNNING)
  {
    s->job = JOB_DONE;
    s->job_code = BX(f);
    kprintf("vm: session %d job done, exit code %d\n", cur, BX(f));
  }
  f->eflags &= ~EFL_CF;
}

/* INT 2Fh AX=DE0Ch: start a job in a free slot: DS:SI command (ASCIZ,
   empty: interactive shell), DX flags. AX = session, CF if none free. */
static void api_job_start(struct frame *f)
{
  const char *cmd = rm_ptr((uint16_t)f->ds, SI(f));
  int n;

  for (n = 1; n < MAX_SESSIONS; n++)
    if (ses[n].used && ses[n].slot && ses[n].job == JOB_NONE)
    {
      struct session *s = &ses[n];
      int i;
      for (i = 0; i < (int)sizeof(s->cmd) - 1 && cmd[i]; i++)
        s->cmd[i] = cmd[i];
      s->cmd[i] = 0;
      s->job_flags = DX(f);
      s->job_code = 0;
      s->job = JOB_ASSIGNED;
      want = n;
      AX(f) = (uint16_t)n;
      f->eflags &= ~EFL_CF;
      return;
    }
  f->eflags |= EFL_CF;
}

/* INT 2Fh AX=DE0Dh: BL = session -> AX = job state (0 none, 1 assigned,
   2 running, 3 done), BX = exit code. Reading "done" frees the slot;
   AX = FFFFh if the session is not a job slot. */
static void api_job_state(struct frame *f)
{
  int n = BL(f);
  struct session *s;

  if (n < 0 || n >= MAX_SESSIONS || !ses[n].used || !ses[n].slot)
  {
    AX(f) = 0xffff;
    f->eflags |= EFL_CF;
    return;
  }
  s = &ses[n];
  AX(f) = (uint16_t)s->job;
  BX(f) = s->job_code;
  if (s->job == JOB_DONE)
    s->job = JOB_NONE;
  f->eflags &= ~EFL_CF;
}

/* INT 2Fh AH=DEh, AL=02h..0Dh; returns 1 if handled */
int vm_api(struct frame *f)
{
  switch (AL(f))
  {
    case 0x02:
      vm_fork(f);
      return 1;
    case 0x03:
      vm_end(f);
      return 1;
    case 0x04:
      if (vm_register(f))
      {
        int i;
        /* session 0 keeps its identity memory but needs shadow video
           pages and an instance buffer */
        struct session *s = &ses[0];
        for (i = 0; i < VRAM_PAGES; i++)
          s->vram[i] = page_alloc();
        for (i = 0; i < (int)((inst_size + 4095) / 4096); i++)
          map_page(INST_BASE + i * 4096, page_alloc(), 3);
        s->inst = INST_BASE;
        f->eflags &= ~EFL_CF;
      }
      else
        f->eflags |= EFL_CF;
      return 1;
    case 0x05:                          /* set focus: BL = session */
      set_focus(BL(f));
      AX(f) = (uint16_t)focus;
      return 1;
    case 0x07:                          /* install the virtual BIOS mouse driver */
      AX(f) = (uint16_t)bios_mouse_install();
      return 1;
    case 0x08: api_screen(f); return 1;
    case 0x09: api_keys(f); return 1;
    case 0x0a: api_job_get(f); return 1;
    case 0x0b: api_job_done(f); return 1;
    case 0x0c: api_job_start(f); return 1;
    case 0x0d: api_job_state(f); return 1;
    case 0x06:                          /* AX = running, BX = focus, CX = count,
                                           DX = 1 if sessions are set up */
      DX(f) = (uint16_t)registered;
      AX(f) = (uint16_t)cur;
      BX(f) = (uint16_t)focus;
      CX(f) = (uint16_t)nsessions;
      return 1;
  }
  return 0;
}

/* INT 2Ah: DOS critical sections */
void vm_crit(struct frame *f)
{
  switch (AH(f))
  {
    case 0x80: crit++; break;
    case 0x81: if (crit) crit--; break;
    case 0x82: crit = 0; break;
  }
}
