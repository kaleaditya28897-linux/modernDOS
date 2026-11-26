/* A plain DOS program for tests/compat: built with Open Watcom for real
   mode and for each DOS extender, and compiled once more on modernDOS
   itself. It does what DOS programs do - memory, files, DOS and BIOS calls,
   the timer - and says so on standard output. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

static int ok, checks;

static void check(int good, const char *what)
{
  checks++;
  ok += good != 0;
  printf("  %s %s\n", good ? "ok  " : "FAIL", what);
}

int main(void)
{
  union REGS r;
  char *p;
  FILE *f;
  char buf[64];
  unsigned long n = sizeof(int) == 4 ? 4ul * 1024 * 1024 : 40000ul, i, sum = 0;

  printf("hello: %u bit program\n", (unsigned)(sizeof(int) * 8));

  /* memory: 4 MB for a 32 bit program, 40000 bytes in real mode */
  p = malloc((size_t)n);
  if (p)
  {
    for (i = 0; i < n; i += 997)
      p[i] = (char)i;
    for (i = 0; i < n; i += 997)
      sum += (unsigned char)p[i] == (unsigned char)i;
  }
  check(p && sum == (n + 996) / 997, "memory: allocated, written, read back");
  free(p);

  /* a file */
  f = fopen("HELLO.TMP", "w");
  if (f)
  {
    fputs("written by hello\n", f);
    fclose(f);
  }
  buf[0] = 0;
  f = fopen("HELLO.TMP", "r");
  if (f)
  {
    fgets(buf, sizeof(buf), f);
    fclose(f);
  }
  remove("HELLO.TMP");
  check(!strcmp(buf, "written by hello\n"), "file: written, read back, deleted");

  /* DOS: version (INT 21h AH=30h) */
  memset(&r, 0, sizeof(r));
  r.h.ah = 0x30;
  intdos(&r, &r);
  check(r.h.al >= 5, "DOS call: version 5 or later");

  /* BIOS: equipment word (INT 11h), the timer ticks (INT 1Ah) */
  memset(&r, 0, sizeof(r));
#ifdef __386__
  int386(0x11, &r, &r);
#else
  int86(0x11, &r, &r);
#endif
  check(1, "BIOS call: INT 11h returned");
  {
    unsigned long t0, t1;
    long spin;
    memset(&r, 0, sizeof(r));
#ifdef __386__
    int386(0x1a, &r, &r);
    t0 = ((unsigned long)r.w.cx << 16) | r.w.dx;
#else
    int86(0x1a, &r, &r);
    t0 = ((unsigned long)r.x.cx << 16) | r.x.dx;
#endif
    t1 = t0;
    for (spin = 0; spin < 40000000L && t1 == t0; spin++)
    {
      if (spin % 1000)
        continue;
      memset(&r, 0, sizeof(r));
#ifdef __386__
      int386(0x1a, &r, &r);
      t1 = ((unsigned long)r.w.cx << 16) | r.w.dx;
#else
      int86(0x1a, &r, &r);
      t1 = ((unsigned long)r.x.cx << 16) | r.x.dx;
#endif
    }
    check(t1 != t0, "timer: the BIOS tick count advances");
  }
  printf("hello: %s %d ok\n", ok == checks ? "all" : "only", ok);
  return ok == checks ? 0 : 1;
}
