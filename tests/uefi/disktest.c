/*
 * DISKTEST - hard disk test for the native disk drivers (ia16).
 *
 *   DISKTEST d:   on drive d:
 *   - BIOS: INT 13h AH=41h extensions, AH=48h parameters for drive 80h
 *   - reads \HELLO.TXT written by Linux (mtools)
 *   - writes \DISKTEST\BIG.BIN (1 MB, pattern) in chunks of varying size,
 *     reads it back in other chunk sizes and compares
 *   - creates 60 small files, deletes every other one
 *   - leaves \FROMDOS.TXT for the harness to check from Linux
 * Logs to port E9h, exit code = number of failures.
 */
#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <string.h>
#include <stdio.h>

#define BIG_SIZE   (1024L * 1024L)

static union REGS r;
static struct SREGS sr;
static int failures, checks;
static char buf[24576];
static char path[80];
static char drive;

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

static int dos(void)
{
  r.x.cflag = 1;
  segread(&sr);
  intdosx(&r, &r, &sr);
  return !r.x.cflag;
}

static const char *p(const char *rest)
{
  sprintf(path, "%c:%s", drive, rest);
  return path;
}

static int fopen_rw(const char *name, int create)
{
  r.x.dx = FP_OFF(name);
  if (create)
  {
    r.h.ah = 0x3c;
    r.x.cx = 0;
  }
  else
    r.x.ax = 0x3d02;
  return dos() ? (int)r.x.ax : -1;
}

static void fclose_h(int h)
{
  r.h.ah = 0x3e;
  r.x.bx = h;
  dos();
}

static unsigned frw(int h, unsigned n, int write)
{
  r.h.ah = write ? 0x40 : 0x3f;
  r.x.bx = h;
  r.x.cx = n;
  r.x.dx = FP_OFF(buf);
  return dos() ? r.x.ax : 0;
}

static unsigned char pattern(long off)
{
  return (unsigned char)(off * 7 + (off >> 9) + (off >> 17));
}

static void fill(long off, unsigned n)
{
  unsigned i;
  for (i = 0; i < n; i++)
    buf[i] = pattern(off + i);
}

static int same(long off, unsigned n)
{
  unsigned i;
  for (i = 0; i < n; i++)
    if ((unsigned char)buf[i] != pattern(off + i))
      return 0;
  return 1;
}

static void bios_checks(void)
{
  static unsigned char params[0x1e];

  r.h.ah = 0x41;
  r.x.bx = 0x55aa;
  r.h.dl = 0x80;
  int86(0x13, &r, &r);
  check(!r.x.cflag && r.x.bx == 0xaa55 && (r.x.cx & 1), "INT 13h AH=41h: extensions on drive 80h");

  memset(params, 0, sizeof(params));
  *(unsigned *)params = sizeof(params);
  r.h.ah = 0x48;
  r.h.dl = 0x80;
  r.x.si = FP_OFF(params);
  segread(&sr);
  int86x(0x13, &r, &r, &sr);
  sprintf(buf, "         drive 80h: %lu sectors of %u bytes\n",
          *(unsigned long *)(params + 16), *(unsigned *)(params + 24));
  out(buf);
  check(!r.x.cflag && *(unsigned long *)(params + 16) >= 65536UL &&
        *(unsigned *)(params + 24) == 512, "INT 13h AH=48h: drive parameters");
}

int main(int argc, char **argv)
{
  static const unsigned wsizes[] = { 512, 4096, 1, 24576, 3000, 17000 };
  static const unsigned rsizes[] = { 24576, 777, 8192, 512, 20000 };
  long off;
  int h, i, ok, n;
  unsigned k;

  drive = argc > 1 ? (char)(argv[1][0] & 0xdf) : 'C';
  sprintf(buf, "\ndisk test on %c:\n", drive);
  out(buf);
  bios_checks();

  h = fopen_rw(p("\\HELLO.TXT"), 0);
  n = h >= 0 ? (int)frw(h, 100, 0) : 0;
  if (h >= 0)
    fclose_h(h);
  check(n >= 16 && memcmp(buf, "hello from linux", 16) == 0, "read HELLO.TXT written by Linux");

  r.h.ah = 0x39;
  r.x.dx = FP_OFF(p("\\DISKTEST"));
  dos();

  /* write 1 MB in varying chunk sizes */
  h = fopen_rw(p("\\DISKTEST\\BIG.BIN"), 1);
  ok = h >= 0;
  for (off = 0, i = 0; ok && off < BIG_SIZE; i++)
  {
    k = wsizes[i % 6];
    if (off + k > BIG_SIZE)
      k = (unsigned)(BIG_SIZE - off);
    fill(off, k);
    ok = frw(h, k, 1) == k;
    off += k;
  }
  if (h >= 0)
    fclose_h(h);
  check(ok, "write 1 MB BIG.BIN");

  /* read it back in other chunk sizes */
  h = fopen_rw(p("\\DISKTEST\\BIG.BIN"), 0);
  ok = h >= 0;
  for (off = 0, i = 0; ok && off < BIG_SIZE; i++)
  {
    k = rsizes[i % 5];
    if (off + k > BIG_SIZE)
      k = (unsigned)(BIG_SIZE - off);
    ok = frw(h, k, 0) == k && same(off, k);
    off += k;
  }
  if (h >= 0)
  {
    ok = ok && frw(h, 10, 0) == 0;      /* at end of file */
    fclose_h(h);
  }
  check(ok, "read back 1 MB and compare");

  /* many small files, delete every other */
  ok = 1;
  for (i = 0; ok && i < 60; i++)
  {
    sprintf(buf, "%c:\\DISKTEST\\F%02d.TXT", drive, i);
    h = fopen_rw(buf, 1);
    ok = h >= 0;
    if (ok)
    {
      n = sprintf(buf, "small file %d\r\n", i);
      ok = frw(h, n, 1) == (unsigned)n;
      fclose_h(h);
    }
  }
  for (i = 0; ok && i < 60; i += 2)
  {
    sprintf(buf, "%c:\\DISKTEST\\F%02d.TXT", drive, i);
    r.h.ah = 0x41;
    r.x.dx = FP_OFF(buf);
    ok = dos();
  }
  check(ok, "create 60 files, delete 30");

  h = fopen_rw(p("\\FROMDOS.TXT"), 1);
  if (h >= 0)
  {
    strcpy(buf, "written by DOS through AHCI\r\n");
    frw(h, strlen(buf), 1);
    fclose_h(h);
  }
  check(h >= 0, "write FROMDOS.TXT");

  /* make sure everything reached the disk */
  r.h.ah = 0x0d;
  dos();

  sprintf(buf, "DISKTEST: %d failures / %d checks\n", failures, checks);
  out(buf);
  return failures;
}
