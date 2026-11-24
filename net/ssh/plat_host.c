/* platform services on Linux, for testing the SSH protocol code against
   OpenSSH without DOS: exec jobs run with /bin/sh, the SFTP root "/C" is
   the directory $SSHD_ROOT (default: the current one); no shells. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "plat.h"
#include "ssh.h"

#define MAX_JOBS 3

static struct { pid_t pid; int state, code; } jobs[MAX_JOBS + 1];
const char *plat_prog = "sshd";
static char root[512];

int plat_init(void)
{
  const char *r = getenv("SSHD_ROOT");

  snprintf(root, sizeof(root), "%s", r ? r : ".");
  return 1;
}

void plat_log(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

uint32_t plat_ms(void)
{
  struct timespec t;

  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

void plat_idle(void)
{
  usleep(2000);
}

/* stdin without blocking (tests pipe the password in) */
int plat_key(void)
{
  static int eof;
  struct pollfd pf = { 0, POLLIN, 0 };
  unsigned char c;

  if (eof || poll(&pf, 1, 0) <= 0)
    return -1;
  if (read(0, &c, 1) != 1)
  {
    eof = 1;
    return -1;
  }
  return c;
}

int plat_bioskey(void)
{
  int c = plat_key();

  if (c == '\n')
    c = '\r';
  return c < 0 ? -1 : c;
}

void plat_screen_show(const uint16_t *cells, int row, int col, int cursor_on)
{
  (void)cells; (void)row; (void)col; (void)cursor_on;
}

void plat_nonblock(int fd)
{
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

int plat_listen(int port)
{
  struct sockaddr_in sa;
  int fd = socket(AF_INET, SOCK_STREAM, 0), on = 1;

  if (fd < 0)
    return -1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0)
  {
    close(fd);
    return -1;
  }
  plat_nonblock(fd);
  return fd;
}

int plat_accept(int lfd, char *peer, size_t n)
{
  struct sockaddr_in sa;
  socklen_t len = sizeof(sa);
  int fd = accept(lfd, (struct sockaddr *)&sa, &len);

  if (fd < 0)
    return -1;
  plat_nonblock(fd);
  snprintf(peer, n, "%s:%u", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
  return fd;
}

int plat_connect(const char *host, int port)
{
  struct addrinfo hints, *ai;
  char ps[8];
  int fd;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(ps, sizeof(ps), "%d", port);
  if (getaddrinfo(host, ps, &hints, &ai))
    return -1;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) < 0)
  {
    freeaddrinfo(ai);
    if (fd >= 0)
      close(fd);
    return -1;
  }
  freeaddrinfo(ai);
  plat_nonblock(fd);
  return fd;
}

void plat_close(int fd)
{
  close(fd);
}

int net_recv(int fd, void *p, int n)
{
  ssize_t r = recv(fd, p, (size_t)n, 0);

  if (r > 0)
    return (int)r;
  if (r == 0)
    return -1;
  return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

int net_send(int fd, const void *p, int n)
{
  ssize_t r = send(fd, p, (size_t)n, MSG_NOSIGNAL);

  if (r >= 0)
    return (int)r;
  return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

int plat_job_start(const char *cmd)
{
  int i;

  if (!*cmd)
    return -1;                          /* no interactive shells here */
  for (i = 1; i <= MAX_JOBS; i++)
    if (jobs[i].state == JOB_NONE)
    {
      pid_t pid = fork();
      if (pid < 0)
        return -1;
      if (!pid)
      {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
      }
      jobs[i].pid = pid;
      jobs[i].state = JOB_RUNNING;
      return i;
    }
  return -1;
}

int plat_job_state(int slot, int *code)
{
  int st;

  if (slot < 1 || slot > MAX_JOBS)
    return -1;
  if (jobs[slot].state == JOB_RUNNING && waitpid(jobs[slot].pid, &st, WNOHANG) == jobs[slot].pid)
  {
    jobs[slot].state = JOB_DONE;
    jobs[slot].code = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
  }
  *code = jobs[slot].code;
  st = jobs[slot].state;
  if (st == JOB_DONE)
    jobs[slot].state = JOB_NONE;
  return st;
}

int plat_screen(int slot, uint8_t cells[4000], int *row, int *col, int *hidden)
{
  (void)slot;
  memset(cells, 0, 4000);
  *row = *col = *hidden = 0;
  return 1;
}

int plat_keys(int slot, const uint16_t *keys, int n)
{
  (void)slot;
  (void)keys;
  return n;
}

int plat_native_path(const char *sftp, char *out, size_t n)
{
  if (strncmp(sftp, "/C", 2) || (sftp[2] && sftp[2] != '/'))
    return 0;
  return snprintf(out, n, "%s%s", root, sftp[2] ? sftp + 2 : "/") < (int)n;
}

int plat_drive_ok(char letter)
{
  return letter == 'C';
}

const char *plat_home(void)
{
  return "/C";
}

const char *plat_tempdir(void)
{
  return "/tmp/";
}
