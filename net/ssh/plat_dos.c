/* platform services on modernDOS: DJGPP, Watt-32, MDVMM monitor calls */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <dir.h>
#include <dos.h>
#include <bios.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <conio.h>
#include <pc.h>
#include <sys/movedata.h>
#include <sys/stat.h>
#include <tcp.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "plat.h"
#include "ssh.h"

const char *plat_prog = "sshd";
static int have_mdvmm;
static char home[16], tmpdir[80];

static int mdvmm(__dpmi_regs *r, int al)
{
  r->h.ah = 0xde;
  r->h.al = (unsigned char)al;
  __dpmi_int(0x2f, r);
  return !(r->x.flags & 1);
}

int plat_init(void)
{
  __dpmi_regs r;
  const char *t;
  char cwd[80];

  memset(&r, 0, sizeof(r));
  r.x.ax = 0xde00;
  __dpmi_int(0x2f, &r);
  have_mdvmm = r.h.al == 0xff;
  if (!getcwd(cwd, sizeof(cwd)))
    strcpy(cwd, ".");
  if (isalpha((unsigned char)cwd[0]) && cwd[1] == ':')
    snprintf(home, sizeof(home), "/%c", toupper((unsigned char)cwd[0]));
  else
    strcpy(home, "/C");
  /* exec output files: absolute, DOS separators (COMMAND.COM redirects) */
  t = getenv("TEMP");
  if (!t)
    t = getenv("TMP");
  snprintf(tmpdir, sizeof(tmpdir), "%s", t ? t : cwd);
  {
    char *p;
    size_t n;
    for (p = tmpdir; *p; p++)
      if (*p == '/')
        *p = '\\';
    n = strlen(tmpdir);
    if (n && tmpdir[n - 1] != '\\' && n + 1 < sizeof(tmpdir))
      strcat(tmpdir, "\\");
  }
  if (sock_init())
  {
    plat_log("network setup failed (packet driver loaded? WATTCP.CFG?)\n");
    return 0;
  }
  {
    char ip[20];
    if (!strcmp(plat_prog, "sshd"))     /* the client stays quiet */
      plat_log("network up, address %s\n", _inet_ntoa(ip, my_ip_addr));
  }
  return 1;
}

void plat_log(const char *fmt, ...)
{
  char buf[300];
  va_list ap;
  size_t n;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fputs(buf, stderr);                   /* stdout is a command's output */
  if (have_mdvmm)                       /* the monitor's log (and port E9h) */
  {
    __dpmi_regs r;
    char line[320];
    char *p;
    snprintf(line, sizeof(line), "%s: %s", plat_prog, buf);
    for (p = line; *p; p++)
      if (*p == '\n')
        *p = '\r';                      /* the log wants CR LF */
    n = strlen(line);
    if (n && line[n - 1] == '\r')
    {
      line[n - 1] = 0;
      strcat(line, "\r\n");
    }
    dosmemput(line, strlen(line) + 1, __tb);
    memset(&r, 0, sizeof(r));
    r.x.ds = __tb >> 4;
    r.x.dx = __tb & 15;
    mdvmm(&r, 0x01);
  }
}

/* milliseconds from the BIOS tick count (55 ms steps). Not uclock(): it
   reprograms the timer, and this session's tick count jumps when MDVMM
   catches up the ticks it missed while other sessions ran. Never goes
   backwards (midnight wrap included). */
uint32_t plat_ms(void)
{
  static uint32_t last, base;
  uint32_t t = _farpeekl(_dos_ds, 0x46c);

  if (t < last)
    base += 0x1800b0;                   /* past midnight */
  last = t;
  return (uint32_t)(((uint64_t)(base + t) * 54925) / 1000);
}

void plat_idle(void)
{
  __dpmi_yield();
}

int plat_key(void)
{
  if (!kbhit())
    return -1;
  return getch();
}

int plat_bioskey(void)
{
  if (!_bios_keybrd(_KEYBRD_READY))
    return -1;
  return (int)(_bios_keybrd(_KEYBRD_READ) & 0xffff);
}

void plat_screen_show(const uint16_t *cells, int row, int col, int cursor_on)
{
  __dpmi_regs r;

  dosmemput(cells, 80 * 25 * 2, 0xb8000);
  memset(&r, 0, sizeof(r));
  r.h.ah = 0x02;                        /* cursor position, page 0 */
  r.h.dh = (unsigned char)row;
  r.h.dl = (unsigned char)col;
  __dpmi_int(0x10, &r);
  memset(&r, 0, sizeof(r));
  r.h.ah = 0x01;                        /* cursor shape */
  r.x.cx = cursor_on ? 0x0607 : 0x2000;
  __dpmi_int(0x10, &r);
}

void plat_nonblock(int fd)
{
  int on = 1;
  ioctlsocket(fd, FIONBIO, (char *)&on);
}

int plat_listen(int port)
{
  struct sockaddr_in sa;
  int fd = socket(AF_INET, SOCK_STREAM, 0), on = 1;

  if (fd < 0)
    return -1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  sa.sin_addr.s_addr = INADDR_ANY;
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0)
  {
    closesocket(fd);
    return -1;
  }
  plat_nonblock(fd);
  return fd;
}

int plat_accept(int lfd, char *peer, size_t n)
{
  struct sockaddr_in sa;
  int len = sizeof(sa), fd;

  fd = accept(lfd, (struct sockaddr *)&sa, &len);
  if (fd < 0)
    return -1;
  plat_nonblock(fd);
  snprintf(peer, n, "%s:%u", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
  return fd;
}

int plat_connect(const char *host, int port)
{
  struct sockaddr_in sa;
  struct hostent *h;
  int fd;

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  sa.sin_addr.s_addr = inet_addr(host);
  if (sa.sin_addr.s_addr == INADDR_NONE)
  {
    h = gethostbyname(host);
    if (!h)
      return -1;
    memcpy(&sa.sin_addr, h->h_addr, sizeof(sa.sin_addr));
  }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
  {
    closesocket(fd);
    return -1;
  }
  plat_nonblock(fd);
  return fd;
}

void plat_close(int fd)
{
  closesocket(fd);
}

int net_recv(int fd, void *p, int n)
{
  int r = recv(fd, p, n, 0);

  if (r > 0)
    return r;
  if (r == 0)
    return -1;
  if (errno == EWOULDBLOCK || errno == EAGAIN)
    return 0;
  return -1;
}

int net_send(int fd, const void *p, int n)
{
  int r = send(fd, (void *)p, n, 0);

  if (r >= 0)
    return r;
  if (errno == EWOULDBLOCK || errno == EAGAIN)
    return 0;
  return -1;
}

/* ---------------------------------------------------------------- */
/* job slots                                                        */

int plat_job_start(const char *cmd)
{
  __dpmi_regs r;

  if (!have_mdvmm)
    return -1;
  memset(&r, 0, sizeof(r));
  dosmemput(cmd, strlen(cmd) + 1, __tb);
  r.x.ds = __tb >> 4;
  r.x.si = __tb & 15;
  return mdvmm(&r, 0x0c) ? r.x.ax : -1;
}

int plat_job_state(int slot, int *code)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  r.h.bl = (unsigned char)slot;
  if (!mdvmm(&r, 0x0d))
    return -1;
  *code = r.x.bx;
  return r.x.ax;
}

int plat_screen(int slot, uint8_t cells[4000], int *row, int *col, int *hidden)
{
  __dpmi_regs r;

  memset(&r, 0, sizeof(r));
  r.h.bl = (unsigned char)slot;
  r.x.es = __tb >> 4;
  r.x.di = __tb & 15;
  r.x.cx = 4000;
  if (!mdvmm(&r, 0x08))
    return 0;
  if (r.x.si < 4000)
    memset(cells, 0, 4000);
  dosmemget(__tb, r.x.si, cells);
  *row = r.h.ah;
  *col = r.h.al;
  *hidden = (r.x.bx & 0x2000) != 0 || (r.h.cl > 3 && r.h.cl != 7);
  return 1;
}

int plat_keys(int slot, const uint16_t *keys, int n)
{
  __dpmi_regs r;

  if (n > 64)
    n = 64;
  memset(&r, 0, sizeof(r));
  dosmemput(keys, (size_t)n * 2, __tb);
  r.h.bl = (unsigned char)slot;
  r.x.ds = __tb >> 4;
  r.x.si = __tb & 15;
  r.x.cx = (unsigned short)n;
  if (!mdvmm(&r, 0x09))
    return 0;
  return r.x.cx;
}

/* ---------------------------------------------------------------- */
/* files                                                            */

/* "/C/DIR/FILE" -> "C:/DIR/FILE"; "/C" -> "C:/" */
int plat_native_path(const char *sftp, char *out, size_t n)
{
  if (sftp[0] != '/' || !isalpha((unsigned char)sftp[1]) || (sftp[2] && sftp[2] != '/'))
    return 0;
  if (snprintf(out, n, "%c:%s", toupper((unsigned char)sftp[1]), sftp[2] ? sftp + 2 : "/") >= (int)n)
    return 0;
  return 1;
}

int plat_drive_ok(char letter)
{
  unsigned int cur, total;
  int d = toupper((unsigned char)letter) - 'A' + 1;

  if (d < 1 || d > 26)
    return 0;
  if (d <= 2)                           /* floppies: only the current one */
  {
    _dos_getdrive(&cur);
    return (int)cur == d;
  }
  {
    __dpmi_regs r;                      /* INT 21h AX=4409h: drive exists? */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4409;
    r.h.bl = (unsigned char)d;
    __dpmi_int(0x21, &r);
    (void)total;
    return !(r.x.flags & 1);
  }
}

const char *plat_home(void)
{
  return home;
}

const char *plat_tempdir(void)
{
  return tmpdir;
}
