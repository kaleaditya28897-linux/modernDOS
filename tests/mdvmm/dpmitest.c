/*
 * MDVMM DPMI host test (DJGPP, 32-bit client).
 *
 * Exercises the host through DJGPP's libc: large memory blocks, exceptions
 * delivered as signals, a protected mode timer interrupt handler, a real
 * mode callback, DOS memory and LDT descriptors. Logs to port E9h; the
 * exit code is the number of failures.
 *
 * Build: i586-pc-msdosdjgpp-gcc -O2 -o dpmitest.exe dpmitest.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>

static int failures, checks;

static void out(const char *s)
{
  for (; *s; s++)
  {
    if (*s == '\n')
      outportb(0xe9, '\r');
    outportb(0xe9, *s);
  }
}

static void check(int ok, const char *what)
{
  checks++;
  if (!ok)
    failures++;
  out(ok ? "  ok   " : "  FAIL ");
  out(what);
  out("\n");
}

static jmp_buf jb;
static void on_signal(int sig)
{
  longjmp(jb, sig);
}

static volatile int ticks;
static void timer_handler(void)
{
  ticks++;
}

static volatile int callback_hits;
static _go32_dpmi_registers cb_regs;
static void rm_callback(_go32_dpmi_registers *r)
{
  callback_hits++;
  r->x.ax = 0x1234;
}

static void t_version(void)
{
  __dpmi_version_ret v;

  check(__dpmi_get_version(&v) == 0, "0400h get version");
  check(v.major == 0 && v.minor == 90, "DPMI 0.90");
  check(v.flags & 1, "32 bit host");
}

static void t_memory(void)
{
  const size_t size = 24u << 20;
  char *p = malloc(size);
  size_t i;
  int ok = 1;

  check(p != NULL, "malloc 24 MB");
  if (!p)
    return;
  for (i = 0; i < size; i += 4096)
    p[i] = (char)(i >> 12);
  for (i = 0; i < size && ok; i += 4096)
    ok = p[i] == (char)(i >> 12);
  check(ok, "touch and verify every page");
  free(p);
}

static void t_exceptions(void)
{
  volatile int zero = 0, num = 7, r = 0;
  int sig;

  signal(SIGSEGV, on_signal);
  if ((sig = setjmp(jb)) == 0)
  {
    *(volatile char *)0xfffffff0u = 1;  /* wraps to monitor memory: #PF */
    check(0, "SIGSEGV raised");
  }
  else
    check(sig == SIGSEGV, "invalid access delivered as SIGSEGV");
  signal(SIGSEGV, SIG_DFL);

  signal(SIGFPE, on_signal);
  if ((sig = setjmp(jb)) == 0)
  {
    r = num / zero;                     /* (1 / x is compiled without DIV) */
    check(0, "SIGFPE raised");
  }
  else
    check(sig == SIGFPE, "divide error delivered as SIGFPE");
  signal(SIGFPE, SIG_DFL);
  (void)r;
}

static void t_timer(void)
{
  _go32_dpmi_seginfo old, h;
  unsigned long t0, n;
  __dpmi_regs regs;

  _go32_dpmi_get_protected_mode_interrupt_vector(8, &old);
  h.pm_offset = (unsigned long)timer_handler;
  h.pm_selector = _go32_my_cs();
  check(_go32_dpmi_chain_protected_mode_interrupt_vector(8, &h) == 0,
        "install PM IRQ0 handler (chained)");

  /* wait for 6 BIOS ticks, mixing pure PM time and real mode DOS calls */
  t0 = _farpeekl(_dos_ds, 0x46c);
  for (n = 0; n < 20000000ul && _farpeekl(_dos_ds, 0x46c) - t0 < 6; n++)
    if (n % 4096 == 0)
    {
      regs.h.ah = 0x2c;                 /* DOS get time */
      __dpmi_int(0x21, &regs);
    }
  check(_farpeekl(_dos_ds, 0x46c) - t0 >= 6, "BIOS ticks advance (chained to RM)");
  check(ticks >= 5, "PM handler saw the timer interrupts");
  _go32_dpmi_set_protected_mode_interrupt_vector(8, &old);
}

static void t_callback(void)
{
  _go32_dpmi_seginfo cb;
  __dpmi_regs r;

  cb.pm_offset = (unsigned long)rm_callback;
  check(_go32_dpmi_allocate_real_mode_callback_retf(&cb, &cb_regs) == 0,
        "0303h allocate real mode callback");
  memset(&r, 0, sizeof(r));
  r.x.cs = cb.rm_segment;
  r.x.ip = cb.rm_offset;
  check(__dpmi_simulate_real_mode_procedure_retf(&r) == 0,
        "0301h call it from real mode");
  check(callback_hits == 1 && r.x.ax == 0x1234, "callback ran in PM, result back");
  _go32_dpmi_free_real_mode_callback(&cb);
}

static void t_dosmem_ldt(void)
{
  int sel, seg, ldt;
  unsigned long base;

  seg = __dpmi_allocate_dos_memory(16, &sel);
  check(seg > 0, "0100h allocate DOS memory");
  if (seg > 0)
  {
    _farpokeb(sel, 5, 0x5a);
    check(_farpeekb(_dos_ds, seg * 16 + 5) == 0x5a, "selector maps the DOS block");
    check(__dpmi_free_dos_memory(sel) == 0, "0101h free DOS memory");
  }

  ldt = __dpmi_allocate_ldt_descriptors(1);
  check(ldt > 0, "0000h allocate descriptor");
  check(__dpmi_set_segment_base_address(ldt, 0xb8000) == 0 &&
        __dpmi_get_segment_base_address(ldt, &base) == 0 && base == 0xb8000,
        "0007h/0006h base round trip");
  check(__dpmi_set_segment_limit(ldt, 0xffff) == 0, "0008h set limit");
  check(__dpmi_free_ldt_descriptor(ldt) == 0, "0001h free descriptor");
}

int main(void)
{
  char buf[80];

  out("\nDPMI host test\n");
  t_version();
  t_memory();
  t_exceptions();
  t_timer();
  t_callback();
  t_dosmem_ldt();
  sprintf(buf, "DPMI: %d failures / %d checks\n", failures, checks);
  out(buf);
  return failures;
}
