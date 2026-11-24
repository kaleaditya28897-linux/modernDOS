/* SFTP server, protocol version 3 (draft-ietf-secsh-filexfer-02), as used
 * by OpenSSH's sftp and scp. DOS drives appear as top level directories:
 * "/C/DOS/FILE.TXT" is C:\DOS\FILE.TXT, "/" lists the drives.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <utime.h>
#include <sys/stat.h>
#include "plat.h"
#include "sftp.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

enum {
  FXP_INIT = 1, FXP_VERSION, FXP_OPEN, FXP_CLOSE, FXP_READ, FXP_WRITE, FXP_LSTAT,
  FXP_FSTAT, FXP_SETSTAT, FXP_FSETSTAT, FXP_OPENDIR, FXP_READDIR, FXP_REMOVE,
  FXP_MKDIR, FXP_RMDIR, FXP_REALPATH, FXP_STAT, FXP_RENAME, FXP_READLINK, FXP_SYMLINK,
  FXP_STATUS = 101, FXP_HANDLE, FXP_DATA, FXP_NAME, FXP_ATTRS, FXP_EXTENDED = 200
};
enum { FX_OK, FX_EOF, FX_NO_SUCH_FILE, FX_PERMISSION_DENIED, FX_FAILURE, FX_BAD_MESSAGE,
       FX_NO_CONNECTION, FX_CONNECTION_LOST, FX_OP_UNSUPPORTED };
#define ATTR_SIZE 1
#define ATTR_PERMISSIONS 4
#define ATTR_ACMODTIME 8
#define PF_READ 1
#define PF_WRITE 2
#define PF_APPEND 4
#define PF_CREAT 8
#define PF_TRUNC 16
#define PF_EXCL 32

#define MAX_HANDLES 16
#define MAX_PATH_LEN 260

struct handle {
  int used;
  int fd;                               /* file, or -1 */
  DIR *dir;                             /* directory */
  int drives;                           /* "/": next drive letter to list */
  char path[MAX_PATH_LEN];              /* SFTP path of the directory */
};

struct sftp {
  struct handle h[MAX_HANDLES];
  int ready;
};

struct sftp *sftp_new(void)
{
  return calloc(1, sizeof(struct sftp));
}

void sftp_free(struct sftp *s)
{
  int i;

  if (!s)
    return;
  for (i = 0; i < MAX_HANDLES; i++)
    if (s->h[i].used)
    {
      if (s->h[i].fd >= 0)
        close(s->h[i].fd);
      if (s->h[i].dir)
        closedir(s->h[i].dir);
    }
  free(s);
}

/* ---------------------------------------------------------------- */

/* resolve an SFTP path (absolute, or relative to the home) into a clean
   absolute one: "/", "/C", "/C/DIR" ... */
static int clean_path(const char *in, char *out, size_t n)
{
  char tmp[MAX_PATH_LEN * 2], *seg, *save;
  size_t len = 0;

  if (in[0] == '/')
    snprintf(tmp, sizeof(tmp), "%s", in);
  else
    snprintf(tmp, sizeof(tmp), "%s/%s", plat_home(), in);
  out[0] = 0;
  for (seg = strtok_r(tmp, "/\\", &save); seg; seg = strtok_r(NULL, "/\\", &save))
  {
    if (!strcmp(seg, ".") || !*seg)
      continue;
    if (!strcmp(seg, ".."))
    {
      char *slash = strrchr(out, '/');
      if (slash)
        *slash = 0;
      len = strlen(out);
      continue;
    }
    if (len + 1 + strlen(seg) + 1 > n)
      return 0;
    out[len++] = '/';
    strcpy(out + len, seg);
    len += strlen(seg);
  }
  if (!len)
    strcpy(out, "/");
  else if (len == 2 && out[1] >= 'a' && out[1] <= 'z')
    out[1] -= 32;                       /* "/c" -> "/C" */
  else if (len > 2 && out[1] >= 'a' && out[1] <= 'z')
    out[1] -= 32;
  return 1;
}

static int native(const char *sftp, char *out, size_t n)
{
  char clean[MAX_PATH_LEN];

  return clean_path(sftp, clean, sizeof(clean)) && plat_native_path(clean, out, n);
}

static void status(struct buf *o, uint32_t id, uint32_t code, const char *msg)
{
  size_t at = o->len;

  put_u32(o, 0);
  put_u8(o, FXP_STATUS);
  put_u32(o, id);
  put_u32(o, code);
  put_cstr(o, msg);
  put_cstr(o, "en");
  poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
}

static uint32_t errno_status(void)
{
  switch (errno)
  {
    case ENOENT: case ENOTDIR: return FX_NO_SUCH_FILE;
    case EACCES: case EPERM: case EROFS: return FX_PERMISSION_DENIED;
  }
  return FX_FAILURE;
}

static void status_errno(struct buf *o, uint32_t id)
{
  uint32_t code = errno_status();

  status(o, id, code, code == FX_NO_SUCH_FILE ? "No such file" :
                      code == FX_PERMISSION_DENIED ? "Permission denied" : strerror(errno));
}

static void put_attrs(struct buf *o, const struct stat *st)
{
  uint32_t perm = S_ISDIR(st->st_mode) ? 040755 : (st->st_mode & S_IWUSR) ? 0100644 : 0100444;

  put_u32(o, ATTR_SIZE | ATTR_PERMISSIONS | ATTR_ACMODTIME);
  put_u64(o, S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size);
  put_u32(o, perm);
  put_u32(o, (uint32_t)st->st_atime);
  put_u32(o, (uint32_t)st->st_mtime);
}

static void put_dir_attrs(struct buf *o)
{
  put_u32(o, ATTR_PERMISSIONS);
  put_u32(o, 040755);
}

/* "ls -l" style line for sftp's ls -l */
static void longname(char *out, size_t n, const char *name, const struct stat *st)
{
  char when[16];
  struct tm *t = localtime(&st->st_mtime);
  int dir = S_ISDIR(st->st_mode);

  if (t)
    strftime(when, sizeof(when), "%b %d %H:%M", t);
  else
    strcpy(when, "Jan 01 00:00");
  snprintf(out, n, "%s 1 dos dos %10lu %s %s", dir ? "drwxr-xr-x" :
           (st->st_mode & S_IWUSR) ? "-rw-r--r--" : "-r--r--r--",
           dir ? 0ul : (unsigned long)st->st_size, when, name);
}

static int new_handle(struct sftp *s)
{
  int i;

  for (i = 0; i < MAX_HANDLES; i++)
    if (!s->h[i].used)
    {
      memset(&s->h[i], 0, sizeof(s->h[i]));
      s->h[i].used = 1;
      s->h[i].fd = -1;
      return i;
    }
  return -1;
}

static void put_handle(struct buf *o, uint32_t id, int h)
{
  size_t at = o->len;
  char hs[8];

  snprintf(hs, sizeof(hs), "%d", h);
  put_u32(o, 0);
  put_u8(o, FXP_HANDLE);
  put_u32(o, id);
  put_cstr(o, hs);
  poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
}

static struct handle *get_handle(struct sftp *s, struct buf *p)
{
  uint32_t n;
  const uint8_t *hs = get_str(p, &n);
  int h = 0;
  uint32_t i;

  if (!hs || !n || n > 3)
    return NULL;
  for (i = 0; i < n; i++)
  {
    if (hs[i] < '0' || hs[i] > '9')
      return NULL;
    h = h * 10 + hs[i] - '0';
  }
  if (h >= MAX_HANDLES || !s->h[h].used)
    return NULL;
  return &s->h[h];
}

/* skip an ATTRS structure in a request; returns the flags (and mtime) */
static uint32_t skip_attrs(struct buf *p, uint32_t *atime, uint32_t *mtime)
{
  uint32_t flags = get_u32(p), i, count;

  if (flags & ATTR_SIZE)
    get_u64(p);
  if (flags & 2)                        /* uid, gid */
  {
    get_u32(p);
    get_u32(p);
  }
  if (flags & ATTR_PERMISSIONS)
    get_u32(p);
  if (flags & ATTR_ACMODTIME)
  {
    *atime = get_u32(p);
    *mtime = get_u32(p);
  }
  if (flags & 0x80000000u)              /* extended */
  {
    count = get_u32(p);
    for (i = 0; i < count && !p->err; i++)
    {
      get_str(p, &flags);
      get_str(p, &flags);
    }
  }
  return flags;
}

/* "/" : the drives */
static int is_root(const char *clean)
{
  return !strcmp(clean, "/");
}

static void do_stat(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  char path[MAX_PATH_LEN], clean[MAX_PATH_LEN], nat[MAX_PATH_LEN];
  struct stat st;
  size_t at = o->len;

  (void)s;
  if (!get_cstr(p, path, sizeof(path)) || !clean_path(path, clean, sizeof(clean)))
  {
    status(o, id, FX_BAD_MESSAGE, "bad path");
    return;
  }
  put_u32(o, 0);
  put_u8(o, FXP_ATTRS);
  put_u32(o, id);
  if (is_root(clean) || (strlen(clean) == 2 && plat_drive_ok(clean[1])))
    put_dir_attrs(o);
  else if (plat_native_path(clean, nat, sizeof(nat)) && !stat(nat, &st))
    put_attrs(o, &st);
  else
  {
    o->len = at;
    status_errno(o, id);
    return;
  }
  poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
}

static void do_realpath(struct buf *p, struct buf *o, uint32_t id)
{
  char path[MAX_PATH_LEN], clean[MAX_PATH_LEN];
  size_t at = o->len;

  if (!get_cstr(p, path, sizeof(path)) || !clean_path(*path ? path : ".", clean, sizeof(clean)))
  {
    status(o, id, FX_BAD_MESSAGE, "bad path");
    return;
  }
  put_u32(o, 0);
  put_u8(o, FXP_NAME);
  put_u32(o, id);
  put_u32(o, 1);
  put_cstr(o, clean);
  put_cstr(o, clean);
  put_u32(o, 0);
  poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
}

static void do_open(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  char path[MAX_PATH_LEN], nat[MAX_PATH_LEN];
  uint32_t pf, at = 0, mt = 0;
  int flags = O_BINARY, h, fd;

  get_cstr(p, path, sizeof(path));
  pf = get_u32(p);
  skip_attrs(p, &at, &mt);
  if (p->err || !native(path, nat, sizeof(nat)))
  {
    status(o, id, FX_NO_SUCH_FILE, "bad path");
    return;
  }
  if ((pf & PF_READ) && (pf & PF_WRITE))
    flags |= O_RDWR;
  else if (pf & PF_WRITE)
    flags |= O_WRONLY;
  else
    flags |= O_RDONLY;
  if (pf & PF_APPEND)
    flags |= O_APPEND;
  if (pf & PF_CREAT)
    flags |= O_CREAT;
  if (pf & PF_TRUNC)
    flags |= O_TRUNC;
  if (pf & PF_EXCL)
    flags |= O_EXCL;
  h = new_handle(s);
  if (h < 0)
  {
    status(o, id, FX_FAILURE, "too many open files");
    return;
  }
  fd = open(nat, flags, 0644);
  if (fd < 0)
  {
    s->h[h].used = 0;
    status_errno(o, id);
    return;
  }
  s->h[h].fd = fd;
  put_handle(o, id, h);
}

static void do_read(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  struct handle *h = get_handle(s, p);
  uint64_t off = get_u64(p);
  uint32_t len = get_u32(p);
  size_t at = o->len;
  int n;

  if (!h || h->fd < 0 || p->err)
  {
    status(o, id, FX_FAILURE, "bad handle");
    return;
  }
  if (len > 32768)
    len = 32768;
  if (lseek(h->fd, (off_t)off, SEEK_SET) < 0)
  {
    status_errno(o, id);
    return;
  }
  put_u32(o, 0);
  put_u8(o, FXP_DATA);
  put_u32(o, id);
  put_u32(o, 0);
  buf_reserve(o, len);
  n = read(h->fd, o->d + o->len, len);
  if (n <= 0)
  {
    o->len = at;
    if (n == 0)
      status(o, id, FX_EOF, "end of file");
    else
      status_errno(o, id);
    return;
  }
  o->len += (size_t)n;
  poke_u32(o->d + at + 9, (uint32_t)n);
  poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
}

static void do_write(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  struct handle *h = get_handle(s, p);
  uint64_t off = get_u64(p);
  uint32_t len;
  const uint8_t *data = get_str(p, &len);

  if (!h || h->fd < 0 || !data)
  {
    status(o, id, FX_FAILURE, "bad handle");
    return;
  }
  if (lseek(h->fd, (off_t)off, SEEK_SET) < 0 || write(h->fd, data, len) != (int)len)
  {
    if (errno == 0)
      errno = ENOSPC;
    status_errno(o, id);
    return;
  }
  status(o, id, FX_OK, "");
}

static void do_fstat(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  struct handle *h = get_handle(s, p);
  struct stat st;
  size_t at = o->len;

  if (!h || h->fd < 0 || fstat(h->fd, &st))
  {
    status(o, id, FX_FAILURE, "bad handle");
    return;
  }
  put_u32(o, 0);
  put_u8(o, FXP_ATTRS);
  put_u32(o, id);
  put_attrs(o, &st);
  poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
}

static void do_opendir(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  char path[MAX_PATH_LEN], clean[MAX_PATH_LEN], nat[MAX_PATH_LEN];
  int h;

  if (!get_cstr(p, path, sizeof(path)) || !clean_path(path, clean, sizeof(clean)))
  {
    status(o, id, FX_BAD_MESSAGE, "bad path");
    return;
  }
  h = new_handle(s);
  if (h < 0)
  {
    status(o, id, FX_FAILURE, "too many open directories");
    return;
  }
  strcpy(s->h[h].path, clean);
  if (is_root(clean))
  {
    s->h[h].drives = 'A';
    put_handle(o, id, h);
    return;
  }
  if (!plat_native_path(clean, nat, sizeof(nat)) || !(s->h[h].dir = opendir(nat)))
  {
    s->h[h].used = 0;
    status_errno(o, id);
    return;
  }
  put_handle(o, id, h);
}

static void do_readdir(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  struct handle *h = get_handle(s, p);
  size_t at = o->len;
  uint32_t count = 0;
  char line[MAX_PATH_LEN + 64];

  if (!h || (!h->dir && !h->drives))
  {
    status(o, id, FX_FAILURE, "bad handle");
    return;
  }
  put_u32(o, 0);
  put_u8(o, FXP_NAME);
  put_u32(o, id);
  put_u32(o, 0);
  if (h->drives)
  {
    while (h->drives <= 'Z' && count < 26)
    {
      char name[2] = { (char)h->drives++, 0 };
      if (!plat_drive_ok(name[0]))
        continue;
      snprintf(line, sizeof(line), "drwxr-xr-x 1 dos dos %10u Jan 01 00:00 %s", 0u, name);
      put_cstr(o, name);
      put_cstr(o, line);
      put_dir_attrs(o);
      count++;
    }
  }
  else
  {
    struct dirent *e;
    while (count < 64 && (e = readdir(h->dir)))
    {
      char nat[MAX_PATH_LEN], sp[MAX_PATH_LEN * 2];
      struct stat st;
      snprintf(sp, sizeof(sp), "%s/%s", h->path, e->d_name);
      if (!plat_native_path(sp, nat, sizeof(nat)) || stat(nat, &st))
        memset(&st, 0, sizeof(st));
      longname(line, sizeof(line), e->d_name, &st);
      put_cstr(o, e->d_name);
      put_cstr(o, line);
      put_attrs(o, &st);
      count++;
    }
  }
  if (!count)
  {
    o->len = at;
    status(o, id, FX_EOF, "end of directory");
    return;
  }
  poke_u32(o->d + at + 9, count);
  poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
}

static void do_close(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  struct handle *h = get_handle(s, p);
  int r = 0;

  if (!h)
  {
    status(o, id, FX_FAILURE, "bad handle");
    return;
  }
  if (h->fd >= 0)
    r = close(h->fd);
  if (h->dir)
    closedir(h->dir);
  h->used = 0;
  if (r)
    status_errno(o, id);
  else
    status(o, id, FX_OK, "");
}

/* REMOVE, RMDIR, MKDIR, RENAME, SETSTAT */
static void do_path_op(int type, struct buf *p, struct buf *o, uint32_t id)
{
  char path[MAX_PATH_LEN], nat[MAX_PATH_LEN], path2[MAX_PATH_LEN], nat2[MAX_PATH_LEN];
  uint32_t at = 0, mt = 0, flags = 0;
  int r = -1;

  get_cstr(p, path, sizeof(path));
  if (type == FXP_RENAME)
    get_cstr(p, path2, sizeof(path2));
  if (type == FXP_MKDIR || type == FXP_SETSTAT)
    flags = skip_attrs(p, &at, &mt);
  if (p->err || !native(path, nat, sizeof(nat)) ||
      (type == FXP_RENAME && !native(path2, nat2, sizeof(nat2))))
  {
    status(o, id, FX_NO_SUCH_FILE, "bad path");
    return;
  }
  errno = 0;
  switch (type)
  {
    case FXP_REMOVE: r = unlink(nat); break;
    case FXP_RMDIR: r = rmdir(nat); break;
    case FXP_MKDIR: r = mkdir(nat, 0755); break;
    case FXP_RENAME: r = rename(nat, nat2); break;
    case FXP_SETSTAT:
      r = 0;
      if (flags & ATTR_ACMODTIME)
      {
        struct utimbuf u;
        u.actime = (time_t)at;
        u.modtime = (time_t)mt;
        r = utime(nat, &u);
      }
      break;
  }
  if (r)
    status_errno(o, id);
  else
    status(o, id, FX_OK, "");
}

static void do_fsetstat(struct sftp *s, struct buf *p, struct buf *o, uint32_t id)
{
  struct handle *h = get_handle(s, p);
  uint32_t at = 0, mt = 0;

  skip_attrs(p, &at, &mt);
  /* times are set by the client with SETSTAT after the close as well */
  status(o, id, h ? FX_OK : FX_FAILURE, h ? "" : "bad handle");
}

int sftp_input(struct sftp *s, struct buf *in, struct buf *o)
{
  int done = 0;

  while (in->len - in->pos >= 4)
  {
    uint32_t len = peek_u32(in->d + in->pos), id = 0;
    struct buf p;
    uint8_t type;

    if (len < 1 || len > 300000)
      return -1;
    if (in->len - in->pos < 4 + len)
      break;
    p.d = in->d + in->pos + 4;
    p.len = len;
    p.cap = 0;
    p.pos = 0;
    p.err = 0;
    in->pos += 4 + len;
    type = get_u8(&p);
    if (type == FXP_INIT)
    {
      size_t at = o->len;
      put_u32(o, 0);
      put_u8(o, FXP_VERSION);
      put_u32(o, 3);
      poke_u32(o->d + at, (uint32_t)(o->len - at - 4));
      s->ready = 1;
      done++;
      continue;
    }
    id = get_u32(&p);
    switch (type)
    {
      case FXP_OPEN: do_open(s, &p, o, id); break;
      case FXP_CLOSE: do_close(s, &p, o, id); break;
      case FXP_READ: do_read(s, &p, o, id); break;
      case FXP_WRITE: do_write(s, &p, o, id); break;
      case FXP_LSTAT:
      case FXP_STAT: do_stat(s, &p, o, id); break;
      case FXP_FSTAT: do_fstat(s, &p, o, id); break;
      case FXP_SETSTAT:
      case FXP_REMOVE:
      case FXP_MKDIR:
      case FXP_RMDIR:
      case FXP_RENAME: do_path_op(type, &p, o, id); break;
      case FXP_FSETSTAT: do_fsetstat(s, &p, o, id); break;
      case FXP_OPENDIR: do_opendir(s, &p, o, id); break;
      case FXP_READDIR: do_readdir(s, &p, o, id); break;
      case FXP_REALPATH: do_realpath(&p, o, id); break;
      default: status(o, id, FX_OP_UNSUPPORTED, "not supported"); break;
    }
    done++;
  }
  buf_consume(in, in->pos);
  return done;
}
