/*
 * CONTEST - remote console check (DJGPP, a DPMI client in session 0).
 *
 * Needs SSHSLOT.COM job slots. Checks that the other sessions keep running
 * while this DPMI program runs, and the monitor's remote console calls:
 *   - an interactive shell job: type "ECHO REMOTE-OK" + Enter, find the
 *     output on its screen (DE08h), type EXIT, the job ends (DE0Dh)
 *   - a command job: "/C" with a failing command reports its exit code
 * Logs to port E9h ("CONTEST: ..."), exit code = number of failures.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <pc.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include <time.h>

static int failures;

static void out(const char *fmt, ...)
{
  char buf[300], *p;
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (p = buf; *p; p++)
  {
    if (*p == '\n')
      outportb(0xe9, '\r');
    outportb(0xe9, *p);
  }
}

static void check(int ok, const char *what)
{
  if (!ok)
    failures++;
  out("CONTEST: %s %s\n", ok ? "ok  " : "FAIL", what);
}

static int api(__dpmi_regs *r, int al)
{
  r->h.ah = 0xde;
  r->h.al = (unsigned char)al;
  __dpmi_int(0x2f, r);
  return !(r->x.flags & 1);
}

static int job_start(const char *cmd)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  dosmemput(cmd, strlen(cmd) + 1, __tb);
  r.x.ds = __tb >> 4;
  r.x.si = __tb & 15;
  return api(&r, 0x0c) ? r.x.ax : -1;
}

static int job_state(int n, int *code)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  r.h.bl = (unsigned char)n;
  api(&r, 0x0d);
  *code = r.x.bx;
  return (short)r.x.ax;
}

/* the screen of session n as text lines */
static int screen(int n, char text[25][81])
{
  static unsigned char cells[80 * 25 * 2];
  __dpmi_regs r;
  int y, x;

  memset(&r, 0, sizeof(r));
  r.h.bl = (unsigned char)n;
  r.x.es = __tb >> 4;
  r.x.di = __tb & 15;
  r.x.cx = sizeof(cells);
  if (!api(&r, 0x08))
    return 0;
  dosmemget(__tb, sizeof(cells), cells);
  for (y = 0; y < 25; y++)
  {
    for (x = 0; x < 80; x++)
      text[y][x] = cells[(y * 80 + x) * 2] ? cells[(y * 80 + x) * 2] : ' ';
    text[y][80] = 0;
  }
  return 1;
}

static int type(int n, const char *s)
{
  unsigned short keys[128];
  __dpmi_regs r;
  int i, len = (int)strlen(s);

  for (i = 0; i < len; i++)
    keys[i] = s[i] == '\r' ? 0x1c0d : (unsigned char)s[i];
  memset(&r, 0, sizeof(r));
  dosmemput(keys, len * 2, __tb);
  r.h.bl = (unsigned char)n;
  r.x.ds = __tb >> 4;
  r.x.si = __tb & 15;
  r.x.cx = (unsigned short)len;
  api(&r, 0x09);
  return r.x.cx == len;
}

/* wait (up to secs) for text on session n's screen */
static int wait_text(int n, const char *what, int secs)
{
  char text[25][81];
  clock_t end = clock() + secs * CLOCKS_PER_SEC;
  int y;

  while (clock() < end)
  {
    if (screen(n, text))
      for (y = 0; y < 25; y++)
        if (strstr(text[y], what))
          return 1;
    __dpmi_yield();
  }
  return 0;
}

static int wait_done(int n, int *code, int secs)
{
  clock_t end = clock() + secs * CLOCKS_PER_SEC;

  while (clock() < end)
  {
    if (job_state(n, code) == 3)
      return 1;
    __dpmi_yield();
  }
  return 0;
}

int main(void)
{
  int n, code = -1;

  out("CONTEST: start\n");
  n = job_start("");
  check(n > 0, "interactive job started in a slot");
  if (n > 0)
  {
    check(wait_text(n, ">", 20), "the shell prompt shows on the slot's screen");
    check(type(n, "ECHO REMOTE-OK\r"), "keys typed into the slot");
    check(wait_text(n, "REMOTE-OK", 20), "the command's output on the slot's screen");
    type(n, "EXIT\r");
    check(wait_done(n, &code, 20), "the shell job ended after EXIT");
  }
  n = job_start("A:\\NOSUCH.EXE");
  check(n > 0, "command job started");
  if (n > 0)
  {
    check(wait_done(n, &code, 20), "the command job ended");
    out("CONTEST: exit code %d\n", code);
  }
  n = job_start("ECHO X > A:\\JOB.TXT");
  check(n > 0 && wait_done(n, &code, 20), "a second command job ran");
  check(fopen("A:\\JOB.TXT", "r") != NULL, "the job's output file exists");
  out("CONTEST: %d failures\n", failures);
  return failures;
}
