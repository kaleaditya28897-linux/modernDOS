/*
 * MDVMM DOS sessions test (ia16).
 *
 *   SESSTEST /BG   background job: appends 300 numbered lines to A:\BG.TXT,
 *                  slowly (runs in a session started with MDVMM NEW), then
 *                  waits until it has been focused and unfocused
 *   SESSTEST /FG   foreground checks (session 0):
 *                  - two sessions exist
 *                  - BG.TXT grows while this program never yields (preemption)
 *                  - focus moves to session 1 and back (Ctrl+Alt+F2/F1 sent
 *                    by the test harness through the QEMU monitor)
 *                  - the background session finishes and ends
 *                  - BG.TXT holds all 300 lines in order, FG.TXT 50 lines
 * Logs to port E9h, exit code = number of failures.
 */
#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <string.h>
#include <stdio.h>

static union REGS r;
static struct SREGS sr;
static int failures, checks;
static char buf[400];

static void out(const char *s)
{
  for (; *s; s++)
  {
    if (*s == '\n')
      outp(0xe9, '\r');
    outp(0xe9, *s);
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

static unsigned long ticks(void)
{
  return *(volatile unsigned long __far *)MK_FP(0x40, 0x6c);
}

static int dos(void)
{
  r.x.cflag = 1;
  intdosx(&r, &r, &sr);
  return !r.x.cflag;
}

/* INT 2Fh AX=DE06h: running session, focus, number of sessions */
static void sessions(unsigned *running, unsigned *focus, unsigned *count)
{
  r.x.ax = 0xde06;
  int86(0x2f, &r, &r);
  *running = r.x.ax;
  *focus = r.x.bx;
  *count = r.x.cx;
}

static int open_file(const char *name, int create)
{
  segread(&sr);
  r.x.dx = FP_OFF(name);
  r.x.ax = 0x3d02;
  if (dos())
    return r.x.ax;
  if (!create)
    return -1;
  r.h.ah = 0x3c;
  r.x.cx = 0;
  r.x.dx = FP_OFF(name);
  return dos() ? (int)r.x.ax : -1;
}

static void close_file(int h)
{
  r.h.ah = 0x3e;
  r.x.bx = h;
  dos();
}

static long file_size(const char *name)
{
  long size;
  int h = open_file(name, 0);

  if (h < 0)
    return -1;
  r.x.ax = 0x4202;
  r.x.bx = h;
  r.x.cx = r.x.dx = 0;
  dos();
  size = (long)r.x.dx << 16 | r.x.ax;
  close_file(h);
  return size;
}

static int append(const char *name, const char *text)
{
  int h = open_file(name, 1);

  if (h < 0)
    return 0;
  r.x.ax = 0x4202;
  r.x.bx = h;
  r.x.cx = r.x.dx = 0;
  dos();
  r.h.ah = 0x40;
  r.x.bx = h;
  r.x.cx = strlen(text);
  segread(&sr);
  r.x.dx = FP_OFF(text);
  dos();
  close_file(h);
  return 1;
}

static int background(void)
{
  int i, seen1 = 0, seen0 = 0;
  unsigned running, focus, count;
  unsigned long t;

  for (i = 0; i < 300; i++)
  {
    sprintf(buf, "bg %03d\r\n", i);
    if (!append("A:\\BG.TXT", buf))
      return 1;
    if (i % 20 == 19)                   /* stretch the job over ~2 s */
      for (t = ticks(); ticks() - t < 2;)
        ;
  }
  /* stay alive until the foreground has moved the focus here and back
     (Ctrl+Alt+F2, Ctrl+Alt+F1), so the hotkey checks have a session 1 */
  for (t = ticks(); ticks() - t < 18 * 60 && !(seen1 && seen0);)
  {
    sessions(&running, &focus, &count);
    if (focus == 1)
      seen1 = 1;
    if (seen1 && focus == 0)
      seen0 = 1;
  }
  return 0;
}

static int foreground(void)
{
  unsigned running, focus, count, i;
  unsigned long t;
  long s0, s1;
  int seen_focus1 = 0, seen_focus0 = 0, ok, h, n;

  out("\nsessions test\n");
  sessions(&running, &focus, &count);
  check(count == 2 && running == 0, "two sessions, session 0 running");

  /* preemption: busy work without ever yielding, BG.TXT must grow */
  s0 = file_size("A:\\BG.TXT");
  for (i = 0; i < 50; i++)
  {
    sprintf(buf, "fg %03u\r\n", i);
    append("A:\\FG.TXT", buf);
    for (t = ticks(); ticks() - t < 1;)
      ;                                 /* spin, no INT 28h / HLT */
  }
  s1 = file_size("A:\\BG.TXT");
  sprintf(buf, "         BG.TXT %ld -> %ld bytes while busy\n", s0, s1);
  out(buf);
  check(s1 > s0 && s1 > 0, "background session ran while foreground was busy");

  /* hotkeys from the harness: focus 1, then back to 0 */
  for (t = ticks(); ticks() - t < 18 * 40 && !(seen_focus1 && seen_focus0);)
  {
    sessions(&running, &focus, &count);
    if (focus == 1)
      seen_focus1 = 1;
    if (seen_focus1 && focus == 0)
      seen_focus0 = 1;
  }
  check(seen_focus1, "Ctrl+Alt+F2 focused session 1");
  check(seen_focus0, "Ctrl+Alt+F1 focused session 0 again");

  /* wait for the background job and its session to finish */
  for (t = ticks(); ticks() - t < 18 * 60; )
  {
    sessions(&running, &focus, &count);
    if (count == 1 && file_size("A:\\BGDONE.TXT") >= 0)
      break;
  }
  check(file_size("A:\\BGDONE.TXT") >= 0, "background job completed");
  check(count == 1, "background session ended");

  /* contents */
  ok = 1;
  h = open_file("A:\\BG.TXT", 0);
  for (i = 0; ok && h >= 0 && i < 300; i++)
  {
    char want[16];
    sprintf(want, "bg %03u\r\n", i);
    r.h.ah = 0x3f;
    r.x.bx = h;
    r.x.cx = 8;
    segread(&sr);
    r.x.dx = FP_OFF(buf);
    ok = dos() && r.x.ax == 8 && memcmp(buf, want, 8) == 0;
  }
  if (h >= 0)
    close_file(h);
  check(h >= 0 && ok && file_size("A:\\BG.TXT") == 300 * 8L, "BG.TXT: 300 lines in order");
  n = (int)(file_size("A:\\FG.TXT") / 8);
  check(n == 50, "FG.TXT: 50 lines");

  sprintf(buf, "SESSIONS: %d failures / %d checks\n", failures, checks);
  out(buf);
  return failures;
}

int main(int argc, char **argv)
{
  if (argc > 1 && (argv[1][1] == 'B' || argv[1][1] == 'b'))
    return background();
  return foreground();
}
