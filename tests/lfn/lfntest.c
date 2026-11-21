/*
 * modernDOS long filename API test.
 *
 * Runs as the shell (SHELL=LFNTEST.EXE) on a test floppy booted in QEMU.
 * Exercises INT 21h AX=71xxh, logs every check to the QEMU debug console
 * (port E9h) and writes "success" or "failure" to A:\RESULT.TXT, then
 * powers the machine off via APM.
 *
 * Build: ia16-elf-gcc -mcmodel=small -Os -o lfntest.exe lfntest.c -li86
 */

#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <string.h>

static int failures, checks;
static union REGS r;
static struct SREGS sr;
static char buf[400], buf2[400];
static unsigned char fd[0x13e];          /* Win95 find data */

/* ---------------- output ---------------- */

static void out(const char *s)
{
  for (; *s; s++)
  {
    if (*s == '\n')
      outp(0xe9, '\r');
    outp(0xe9, *s);
  }
}

static void outhex(unsigned v)
{
  static const char hx[] = "0123456789ABCDEF";
  char t[5];
  int i;
  for (i = 3; i >= 0; i--, v >>= 4)
    t[i] = hx[v & 15];
  t[4] = 0;
  out(t);
}

static void check(int ok, const char *what)
{
  checks++;
  if (!ok)
    failures++;
  out(ok ? "  ok   " : "  FAIL ");
  out(what);
  if (!ok)
  {
    out("  (CF=");
    out(r.x.cflag ? "1" : "0");
    out(" AX=");
    outhex(r.x.ax);
    out(")");
  }
  out("\n");
}

static void checkstr(const char *got, const char *want, const char *what)
{
  int ok = strcmp(got, want) == 0;
  check(ok, what);
  if (!ok)
  {
    out("         got  \"");
    out(got);
    out("\"\n         want \"");
    out(want);
    out("\"\n");
  }
}

/* ---------------- INT 21h wrappers ---------------- */

static int dos(void)
{
  r.x.cflag = 1;
  intdosx(&r, &r, &sr);
  return !r.x.cflag;
}

static void ds_dx(const char *p)
{
  segread(&sr);
  r.x.dx = FP_OFF(p);
}

static int lfn_mkdir(const char *p)
{
  r.x.ax = 0x7139;
  ds_dx(p);
  return dos();
}

static int lfn_rmdir(const char *p)
{
  r.x.ax = 0x713a;
  ds_dx(p);
  return dos();
}

static int lfn_chdir(const char *p)
{
  r.x.ax = 0x713b;
  ds_dx(p);
  return dos();
}

static int lfn_getcwd(char *out_)
{
  r.x.ax = 0x7147;
  r.h.dl = 0;
  segread(&sr);
  r.x.si = FP_OFF(out_);
  return dos();
}

/* returns handle or -1; action in *act */
static int lfn_open(const char *p, unsigned mode, unsigned action, int *act)
{
  r.x.ax = 0x716c;
  r.x.bx = mode;
  r.x.cx = 0;
  r.x.dx = action;
  segread(&sr);
  r.x.si = FP_OFF(p);
  r.x.di = 0;
  if (!dos())
    return -1;
  if (act)
    *act = r.x.cx;
  return r.x.ax;
}

static int dos_write(int h, const char *data, unsigned n)
{
  r.h.ah = 0x40;
  r.x.bx = h;
  r.x.cx = n;
  ds_dx(data);
  return dos() && r.x.ax == n;
}

static int dos_read(int h, char *data, unsigned n)
{
  r.h.ah = 0x3f;
  r.x.bx = h;
  r.x.cx = n;
  ds_dx(data);
  return dos() ? (int)r.x.ax : -1;
}

static void dos_close(int h)
{
  r.h.ah = 0x3e;
  r.x.bx = h;
  dos();
}

static int lfn_findfirst(const char *spec, unsigned attr)
{
  r.x.ax = 0x714e;
  r.x.cx = attr;
  r.x.si = 1;                   /* DOS date/time format */
  segread(&sr);
  r.x.dx = FP_OFF(spec);
  r.x.di = FP_OFF(fd);
  sr.es = sr.ds;
  return dos() ? (int)r.x.ax : -1;
}

static int lfn_findnext(int h)
{
  r.x.ax = 0x714f;
  r.x.bx = h;
  r.x.si = 1;
  segread(&sr);
  r.x.di = FP_OFF(fd);
  sr.es = sr.ds;
  return dos();
}

static void lfn_findclose(int h)
{
  r.x.ax = 0x71a1;
  r.x.bx = h;
  dos();
}

#define FD_LONG  ((char *)fd + 0x2c)
#define FD_SHORT ((char *)fd + 0x130)

static int lfn_truename(const char *in, char *out_, int mode)
{
  r.x.ax = 0x7160;
  r.x.cx = mode;
  segread(&sr);
  r.x.si = FP_OFF(in);
  r.x.di = FP_OFF(out_);
  sr.es = sr.ds;
  return dos();
}

static int lfn_rename(const char *from, const char *to)
{
  r.x.ax = 0x7156;
  segread(&sr);
  r.x.dx = FP_OFF(from);
  r.x.di = FP_OFF(to);
  sr.es = sr.ds;
  return dos();
}

static int lfn_delete(const char *p, int wild)
{
  r.x.ax = 0x7141;
  r.x.si = wild;
  r.x.cx = 0;
  ds_dx(p);
  return dos();
}

/* count matches of a spec, using 714E/714F */
static int count_matches(const char *spec, unsigned attr)
{
  int h = lfn_findfirst(spec, attr), n = 0;
  if (h < 0)
    return 0;
  do
    n++;
  while (lfn_findnext(h));
  lfn_findclose(h);
  return n;
}

static int create_file(const char *name, const char *data)
{
  int act, h = lfn_open(name, 0x0002, 0x0012, &act);   /* r/w, create/truncate */
  if (h < 0)
    return 0;
  if (!dos_write(h, data, strlen(data)))
    return 0;
  dos_close(h);
  return 1;
}

static int read_file(const char *name, char *data, unsigned max)
{
  int n, h = lfn_open(name, 0x0000, 0x0001, 0);         /* r/o, open */
  if (h < 0)
    return -1;
  n = dos_read(h, data, max - 1);
  dos_close(h);
  if (n >= 0)
    data[n] = 0;
  return n;
}

/* ---------------- tests ---------------- */

static void t_volume(void)
{
  static char root[] = "A:\\";
  out("volume information (71A0)\n");
  r.x.ax = 0x71a0;
  r.x.cx = sizeof(buf);
  segread(&sr);
  r.x.dx = FP_OFF(root);
  r.x.di = FP_OFF(buf);
  sr.es = sr.ds;
  check(dos(), "71A0 succeeds");
  check((r.x.bx & 0x4000) != 0, "reports LFN support (BX bit 14)");
  check(r.x.cx == 255 && r.x.dx == 260, "max name 255 / path 260");
  checkstr(buf, "FAT", "file system name");
}

static void t_linux_file(void)
{
  out("file created on Linux by mtools\n");
  check(read_file("A:\\Created On Linux.txt", buf, sizeof(buf)) > 0,
        "open long name written by Linux");
  checkstr(buf, "hello from linux\n", "content");
  check(read_file("a:\\CREATED ON LINUX.TXT", buf, sizeof(buf)) > 0,
        "case-insensitive long name lookup");
}

static void t_dirs(void)
{
  out("directories (7139/713B/7147)\n");
  check(lfn_mkdir("A:\\Long Directory Name"), "mkdir long name");
  check(!lfn_mkdir("A:\\long directory name"), "mkdir duplicate fails");
  check(lfn_chdir("A:\\long directory name"), "chdir, different case");
  check(lfn_getcwd(buf), "getcwd");
  checkstr(buf, "Long Directory Name", "getcwd returns long name");
  check(lfn_mkdir("Nested Folder"), "mkdir relative");
  check(lfn_chdir("Nested Folder"), "chdir relative");
  check(lfn_getcwd(buf), "getcwd nested");
  checkstr(buf, "Long Directory Name\\Nested Folder", "nested long cwd");
  check(lfn_chdir(".."), "chdir ..");
}

static void t_files(void)
{
  int h, n;

  out("files (716C/714E/7160)\n");
  check(create_file("My Document With A Long Name.txt", "first file\n"),
        "create long file name");
  check(create_file("My Document With A Long Name 2.txt", "second file\n"),
        "create second, same prefix");
  check(create_file("readme.txt", "case\n"), "create lower case 8.3 name");

  h = lfn_findfirst("My Document With A Long Name.txt", 0);
  check(h >= 0, "findfirst exact long name");
  checkstr(FD_LONG, "My Document With A Long Name.txt", "long name");
  checkstr(FD_SHORT, "MYDOCU~1.TXT", "generated short name");
  if (h >= 0)
    lfn_findclose(h);

  h = lfn_findfirst("*name 2.TXT", 0);
  check(h >= 0, "findfirst wildcard on long name");
  checkstr(FD_SHORT, "MYDOCU~2.TXT", "numeric tail increments");
  if (h >= 0)
    lfn_findclose(h);

  h = lfn_findfirst("readme.txt", 0);
  check(h >= 0, "findfirst lower case name");
  checkstr(FD_LONG, "readme.txt", "case preserved");
  if (h >= 0)
    lfn_findclose(h);

  n = count_matches("*.txt", 0);
  check(n == 3, "714E/714F enumerate 3 files");
  n = count_matches("*", 0x10);
  check(n == 6, "'*' with directories: . .. Nested + 3 files");

  check(read_file("my document with a long name.txt", buf, sizeof(buf)) > 0,
        "open existing long name");
  checkstr(buf, "first file\n", "read back");
  check(read_file("MYDOCU~2.TXT", buf, sizeof(buf)) > 0, "open by short alias");
  checkstr(buf, "second file\n", "alias reaches same file");

  check(lfn_truename("My Document With A Long Name.txt", buf, 1),
        "7160 CL=1 short path");
  checkstr(buf, "A:\\LONGDI~1\\MYDOCU~1.TXT", "short path");
  check(lfn_truename("A:\\LONGDI~1\\MYDOCU~1.TXT", buf, 2),
        "7160 CL=2 long path");
  checkstr(buf, "A:\\Long Directory Name\\My Document With A Long Name.txt",
           "long path");
  check(lfn_truename("Not There Yet.dat", buf, 0), "7160 CL=0 missing file");
  checkstr(buf, "A:\\Long Directory Name\\Not There Yet.dat",
           "canonical name keeps missing component");

  /* classic 8.3 API sees the aliases */
  r.h.ah = 0x1a;                /* set DTA */
  ds_dx(buf2);
  dos();
  r.h.ah = 0x4e;
  r.x.cx = 0;
  ds_dx("MYDOCU~1.TXT");
  check(dos(), "classic 4Eh finds short alias");
}

static void t_rename_attr(void)
{
  int h;

  out("rename and attributes (7156/7143)\n");
  check(lfn_rename("readme.txt", "Read Me Please.txt"), "rename to long name");
  check(lfn_findfirst("readme.txt", 0) < 0, "old name gone");
  h = lfn_findfirst("read me please.txt", 0);
  check(h >= 0, "new name found");
  if (h >= 0)
    lfn_findclose(h);

  check(lfn_rename("Read Me Please.txt", "READ ME PLEASE.TXT"),
        "case-only rename");
  h = lfn_findfirst("read me please.txt", 0);
  checkstr(h >= 0 ? FD_LONG : "", "READ ME PLEASE.TXT", "new case stored");
  if (h >= 0)
    lfn_findclose(h);

  check(lfn_rename("READ ME PLEASE.TXT", "Nested Folder\\Moved File.txt"),
        "move into subdirectory with new long name");
  check(read_file("Nested Folder\\Moved File.txt", buf, sizeof(buf)) > 0,
        "moved file readable");

  r.x.ax = 0x7143;
  r.h.bl = 0;
  ds_dx("My Document With A Long Name.txt");
  check(dos() && (r.x.cx & 0x20), "7143 get attributes (archive)");
  r.x.ax = 0x7143;
  r.h.bl = 1;
  r.x.cx = 0x21;
  ds_dx("My Document With A Long Name.txt");
  check(dos(), "7143 set read-only");
  check(!lfn_delete("My Document With A Long Name.txt", 0),
        "read-only file cannot be deleted");
  r.x.ax = 0x7143;
  r.h.bl = 1;
  r.x.cx = 0x20;
  ds_dx("My Document With A Long Name.txt");
  check(dos(), "7143 clear read-only");
  r.x.ax = 0x7143;
  r.h.bl = 4;
  ds_dx("My Document With A Long Name.txt");
  check(dos() && r.x.di != 0, "7143 get last write date");
}

static void t_many(void)
{
  static char name[] = "Lots Of Files With Quite Long Names 00.txt";
  int i, ok = 1;

  out("directory growth with many long names\n");
  check(lfn_mkdir("Many") && lfn_chdir("Many"), "mkdir and chdir");
  for (i = 0; i < 60 && ok; i++)
  {
    name[36] = '0' + i / 10;
    name[37] = '0' + i % 10;
    ok = create_file(name, "x");
  }
  check(ok, "create 60 files with 4 LFN entries each");
  check(count_matches("Lots Of Files*", 0) == 60, "all 60 enumerate");
  check(count_matches("LOTSO*.TXT", 0) == 60, "short aliases match too");
  {
    int h = lfn_findfirst("Lots Of Files With Quite Long Names 09.txt", 0);
    checkstr(h >= 0 ? FD_SHORT : "", "LOTSO~10.TXT",
             "10th alias shortens base for 2-digit tail");
    if (h >= 0)
      lfn_findclose(h);
  }
  check(lfn_delete("Lots Of Files*", 1), "wildcard delete");
  check(count_matches("*", 0) == 0, "directory empty again");
  check(lfn_chdir(".."), "chdir ..");
  check(lfn_rmdir("Many"), "rmdir");
}

static void t_misc(void)
{
  static unsigned char ft[8];
  unsigned date = (46 << 9) | (11 << 5) | 21;     /* 2026-11-21 */
  unsigned time = (13 << 11) | (37 << 5) | 21;    /* 13:37:42 */

  out("conversions (71A7/71A8)\n");
  r.x.ax = 0x71a7;
  r.h.bl = 1;
  r.x.cx = time;
  r.x.dx = date;
  r.h.bh = 50;
  segread(&sr);
  sr.es = sr.ds;
  r.x.di = FP_OFF(ft);
  check(dos(), "DOS -> FILETIME");
  r.x.ax = 0x71a7;
  r.h.bl = 0;
  segread(&sr);
  r.x.si = FP_OFF(ft);
  check(dos() && r.x.cx == time && r.x.dx == date && r.h.bh == 50,
        "FILETIME -> DOS round trip");

  r.x.ax = 0x71a8;
  r.x.dx = 0x0100;              /* DH=1: ASCIZ */
  segread(&sr);
  sr.es = sr.ds;
  r.x.si = FP_OFF("My Long File.text");
  r.x.di = FP_OFF(buf);
  check(dos(), "71A8 generate short name");
  checkstr(buf, "MYLONGFI.TEX", "basis name");

  r.x.ax = 0x7100 | 0x99;       /* unknown subfunction */
  check(!dos() && r.x.ax == 0x7100, "unknown 71xx -> AX=7100h, CF");
}

/* files left on the disk for the Linux side of the test */
static void t_keep(void)
{
  out("files kept for host verification\n");
  check(create_file("A:\\Long Directory Name\\Keep This File.txt",
                    "written by modernDOS\n"), "create kept file");
  check(lfn_delete("My Document With A Long Name 2.txt", 0), "delete by long name");
}

/* run A:\DJTEST.EXE (DJGPP libc test) if present, expect exit code 0 */
static void t_djgpp(void)
{
  static char prog[] = "A:\\DJTEST.EXE", tail[] = "\0\r";
  static unsigned pb[7];
  unsigned psp, ds;

  r.x.ax = 0x4300;              /* present? */
  ds_dx(prog);
  if (!dos())
  {
    out("DJGPP test not on disk, skipped\n");
    return;
  }
  out("running DJGPP libc test (EXEC)\n");

  /* small model: give back everything above DS:FFFF so EXEC has memory */
  r.h.ah = 0x62;
  dos();
  psp = r.x.bx;
  segread(&sr);
  ds = sr.ds;
  r.h.ah = 0x4a;
  r.x.bx = (ds - psp) + 0x1000;
  sr.es = psp;
  dos();

  pb[0] = 0;                    /* inherit environment */
  pb[1] = FP_OFF(tail);
  pb[2] = ds;
  pb[3] = pb[5] = 0xffff;       /* no FCBs */
  pb[4] = pb[6] = 0xffff;
  r.x.ax = 0x4b00;
  segread(&sr);
  sr.es = sr.ds;
  r.x.bx = FP_OFF(pb);
  r.x.dx = FP_OFF(prog);
  check(dos(), "EXEC DJTEST.EXE");
  r.h.ah = 0x4d;
  dos();
  check(r.h.al == 0, "DJGPP test reported no failures");
}

static void finish(void)
{
  static const char ok_[] = "success", bad[] = "failure";
  int h;

  out(failures ? "RESULT: FAILED " : "RESULT: PASSED ");
  outhex(failures);
  out(" failures / ");
  outhex(checks);
  out(" checks\n");

  r.h.ah = 0x3c;                /* classic create, root */
  r.x.cx = 0;
  ds_dx("A:\\RESULT.TXT");
  if (dos())
  {
    h = r.x.ax;
    dos_write(h, failures ? bad : ok_, failures ? sizeof(bad) - 1 : sizeof(ok_) - 1);
    dos_close(h);
  }
  r.x.ax = 0x0d00;              /* flush */
  dos();

  /* APM power off */
  r.x.ax = 0x5301; r.x.bx = 0; int86(0x15, &r, &r);
  r.x.ax = 0x530e; r.x.bx = 0; r.x.cx = 0x0102; int86(0x15, &r, &r);
  r.x.ax = 0x5307; r.x.bx = 1; r.x.cx = 3; int86(0x15, &r, &r);
  for (;;)
    ;
}

int main(void)
{
  out("\nmodernDOS LFN test\n");
  t_volume();
  t_linux_file();
  t_dirs();
  t_files();
  t_rename_attr();
  t_many();
  t_misc();
  t_keep();
  lfn_chdir("A:\\");
  t_djgpp();
  finish();
  return 0;
}
