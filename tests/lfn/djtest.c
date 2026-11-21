/*
 * modernDOS long filename test using a real libc (DJGPP, 32-bit DPMI).
 *
 * Uses only portable C/POSIX file calls; DJGPP's libc maps them to the
 * INT 21h AX=71xxh API when the kernel reports LFN support.  Logs to the
 * QEMU debug console (port E9h); the exit code is the number of failures.
 *
 * Build: i586-pc-msdosdjgpp-gcc -O2 -o djtest.exe djtest.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pc.h>

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

static void checkstr(const char *got, const char *want, const char *what)
{
  int ok = got && strcmp(got, want) == 0;
  check(ok, what);
  if (!ok)
  {
    out("         got  \"");
    out(got ? got : "(null)");
    out("\"\n         want \"");
    out(want);
    out("\"\n");
  }
}

static int write_file(const char *name, const char *text)
{
  FILE *f = fopen(name, "wb");
  if (!f)
    return 0;
  fputs(text, f);
  return fclose(f) == 0;
}

static int file_has(const char *name, const char *text)
{
  char buf[200];
  FILE *f = fopen(name, "rb");
  size_t n;
  if (!f)
    return 0;
  n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  return strcmp(buf, text) == 0;
}

static int dir_contains(const char *dir, const char *name)
{
  DIR *d = opendir(dir);
  struct dirent *e;
  int found = 0;
  if (!d)
    return 0;
  while ((e = readdir(d)) != NULL)
    if (strcmp(e->d_name, name) == 0)
      found = 1;
  closedir(d);
  return found;
}

int main(void)
{
  char cwd[300], buf[300];
  struct stat st;
  const char *tail;

  out("\nDJGPP libc LFN test\n");
  check(_use_lfn(NULL), "libc detects LFN support");

  check(mkdir("A:/DJGPP Long Folder Name", 0777) == 0, "mkdir long name");
  check(chdir("A:/DJGPP Long Folder Name") == 0, "chdir long name");
  check(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd");
  tail = strchr(cwd, '/');
  checkstr(tail, "/DJGPP Long Folder Name", "getcwd keeps long name");

  check(write_file("A Source File With A Really Long Name.c", "int x;\n"),
        "fopen/write long name");
  check(file_has("a source file with a really long name.C", "int x;\n"),
        "fopen/read, different case");
  check(stat("A Source File With A Really Long Name.c", &st) == 0 &&
        S_ISREG(st.st_mode) && st.st_size == 7, "stat size and type");
  check(write_file("MixedCase.TXT", "m\n"), "create mixed case name");
  check(dir_contains(".", "A Source File With A Really Long Name.c"),
        "readdir returns long name");
  check(dir_contains(".", "MixedCase.TXT"), "readdir preserves case");

  check(rename("A Source File With A Really Long Name.c",
               "Renamed To Another Long Name.txt") == 0, "rename");
  check(access("A Source File With A Really Long Name.c", F_OK) != 0,
        "old name gone");
  check(file_has("Renamed To Another Long Name.txt", "int x;\n"),
        "content follows rename");

  check(mkdir("Sub Folder", 0777) == 0 &&
        write_file("Sub Folder/Deep File Name.dat", "deep\n"),
        "relative path with long components");
  check(file_has("A:/DJGPP Long Folder Name/Sub Folder/Deep File Name.dat",
                 "deep\n"), "absolute path with long components");
  check(_truename("Sub Folder/Deep File Name.dat", buf) != NULL,
        "_truename");
  check(strcasecmp(buf, "A:\\DJGPP Long Folder Name\\Sub Folder\\Deep File Name.dat") == 0,
        "_truename returns long path");

  check(unlink("Sub Folder/Deep File Name.dat") == 0, "unlink long name");
  check(rmdir("Sub Folder") == 0, "rmdir long name");
  check(unlink("MixedCase.TXT") == 0, "unlink mixed case");

  {
    int fd = open("Exclusive Create Test.bin", O_CREAT | O_EXCL | O_WRONLY, 0666);
    check(fd >= 0, "open O_CREAT|O_EXCL");
    if (fd >= 0)
      close(fd);
    check(open("Exclusive Create Test.bin", O_CREAT | O_EXCL | O_WRONLY, 0666) < 0,
          "O_EXCL on existing name fails");
    check(unlink("Exclusive Create Test.bin") == 0, "cleanup");
  }

  sprintf(buf, "DJGPP: %d failures / %d checks\n", failures, checks);
  out(buf);
  return failures;
}
