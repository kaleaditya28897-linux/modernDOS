/*
 * PKG - the modernDOS package manager
 *
 *   PKG UPDATE            fetch the repository's package list
 *   PKG LIST              packages of the repository, [*] = installed
 *   PKG INSTALL name      download, check and unpack a package
 *   PKG REMOVE name       delete an installed package's files
 *   PKG INFO              repository, key, where packages go
 *
 * PKG.CFG (next to PKG.EXE, or in %PKGDIR%):
 *   repo = http://host/path/        the repository
 *   key  = 64 hex digits            its ed25519 public key
 *   root = C:\                      where packages are unpacked
 *
 * A repository (made with scripts/mkrepo.sh) is a directory on a web
 * server: INDEX.TXT lists the packages (name|version|file|size|sha256|
 * description), INDEX.SIG is its ed25519 signature, and each package is an
 * uncompressed tar archive. There is no TLS: the list is only used if the
 * signature fits the key in PKG.CFG, and a package only if its SHA-256 is
 * the one in the list. What a package installed is kept in
 * <root>\MDOS\PKG\<name>.LST.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../ssh/crypto/monocypher-ed25519.h"
#include "../ssh/sha256.h"
#include "http.h"

#ifdef __DJGPP__
#define SEP '\\'
#else
#define SEP '/'
#endif

struct pkg {
  char name[32], version[24], file[64], sha[65], desc[128];
  long size;
};

static char repo[256], root[128], dbdir[160], cfgdir[128];
static uint8_t repo_key[32];
static int have_key;
static struct pkg *pkgs;
static int npkgs;

static void die(const char *fmt, const char *arg)
{
  fprintf(stderr, "PKG: ");
  fprintf(stderr, fmt, arg);
  fprintf(stderr, "\n");
  exit(1);
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
  size_t l = strlen(dir);

  snprintf(out, n, "%s%s%s", dir, l && dir[l - 1] != '/' && dir[l - 1] != '\\' ? (SEP == '/' ? "/" : "\\") : "",
           name);
}

static void make_dirs(const char *path)
{
  char tmp[300], *p;

  snprintf(tmp, sizeof(tmp), "%s", path);
  for (p = tmp + 1; *p; p++)
    if ((*p == '/' || *p == '\\') && p[-1] != ':')
    {
      char c = *p;
      *p = 0;
      mkdir(tmp, 0755);
      *p = c;
    }
}

static int hex(const char *s, uint8_t *out, int n)
{
  int i;
  unsigned v;

  for (i = 0; i < n; i++)
  {
    if (sscanf(s + i * 2, "%2x", &v) != 1)
      return 0;
    out[i] = (uint8_t)v;
  }
  return 1;
}

static void config(const char *argv0)
{
  char path[200], line[400], *slash;
  FILE *f;

  if (getenv("PKGDIR"))
    snprintf(cfgdir, sizeof(cfgdir), "%s", getenv("PKGDIR"));
  else
  {
    snprintf(cfgdir, sizeof(cfgdir), "%s", argv0);
    slash = strrchr(cfgdir, '/');
    if (strrchr(cfgdir, '\\') > slash)
      slash = strrchr(cfgdir, '\\');
    if (slash)
      slash[1] = 0;
    else
      cfgdir[0] = 0;
  }
#ifdef __DJGPP__
  strcpy(root, access("C:/", 0) == 0 ? "C:\\" : "A:\\");
#else
  strcpy(root, "./");
#endif
  join(path, sizeof(path), cfgdir, "PKG.CFG");
  f = fopen(path, "r");
  while (f && fgets(line, sizeof(line), f))
  {
    char key[32], val[300];
    if (sscanf(line, " %31[a-z] = %299[^\r\n]", key, val) != 2)
      continue;
    if (!strcmp(key, "repo"))
      snprintf(repo, sizeof(repo), "%s%s", val, val[strlen(val) - 1] == '/' ? "" : "/");
    else if (!strcmp(key, "key"))
      have_key = strlen(val) >= 64 && hex(val, repo_key, 32);
    else if (!strcmp(key, "root"))
      snprintf(root, sizeof(root), "%s", val);
  }
  if (f)
    fclose(f);
  join(line, sizeof(line), root, "MODERNDOS");
  join(dbdir, sizeof(dbdir), line, "PKG");
}

/* ---------------------------------------------------------------- */

struct mem {
  char *d;
  size_t len, cap;
};

static int to_mem(void *ctx, const void *data, size_t n)
{
  struct mem *m = ctx;

  if (m->len + n + 1 > m->cap)
  {
    m->cap = (m->len + n + 1) * 2;
    m->d = realloc(m->d, m->cap);
    if (!m->d)
      return 1;
  }
  memcpy(m->d + m->len, data, n);
  m->len += n;
  m->d[m->len] = 0;
  return 0;
}

struct dl {
  FILE *f;
  struct sha256 sha;
  long got;
};

static int to_file(void *ctx, const void *data, size_t n)
{
  struct dl *d = ctx;

  sha256_update(&d->sha, data, n);
  d->got += (long)n;
  return fwrite(data, 1, n, d->f) != n;
}

static void need_repo(void)
{
  if (!repo[0] || !have_key)
    die("no repository: put \"repo = http://...\" and \"key = ...\" into %sPKG.CFG", cfgdir);
  if (!http_init())
    die("%s", "no network (packet driver loaded?)");
}

static int parse_index(char *text)
{
  char *line, *save;
  int n = 0;

  for (line = text; *line; line++)
    n += *line == '\n';
  pkgs = calloc((size_t)n + 1, sizeof(*pkgs));
  npkgs = 0;
  for (line = strtok_r(text, "\r\n", &save); line && pkgs; line = strtok_r(NULL, "\r\n", &save))
  {
    struct pkg *p = &pkgs[npkgs];
    if (line[0] == '#')
      continue;
    if (sscanf(line, "%31[^|]|%23[^|]|%63[^|]|%ld|%64[0-9a-f]|%127[^\n]", p->name, p->version, p->file,
               &p->size, p->sha, p->desc) >= 5)
      npkgs++;
  }
  return npkgs;
}

static void load_index(void)
{
  char path[220];
  struct mem m = { 0, 0, 0 };
  FILE *f;
  char buf[1024];
  size_t n;

  join(path, sizeof(path), dbdir, "INDEX.TXT");
  f = fopen(path, "rb");
  if (!f)
    die("%s", "no package list yet: run PKG UPDATE");
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    to_mem(&m, buf, n);
  fclose(f);
  parse_index(m.d ? m.d : "");
}

static struct pkg *find(const char *name)
{
  int i;

  for (i = 0; i < npkgs; i++)
    if (!strcasecmp(pkgs[i].name, name))
      return &pkgs[i];
  return 0;
}

static void list_path(char *out, size_t n, const char *name)
{
  char file[48];

  snprintf(file, sizeof(file), "%.8s.LST", name);
  join(out, n, dbdir, file);
}

static int installed(const char *name, char *version, size_t vn)
{
  char path[220];
  FILE *f;

  list_path(path, sizeof(path), name);
  f = fopen(path, "r");
  if (!f)
    return 0;
  if (version && (!fgets(version, (int)vn, f)))
    version[0] = 0;
  if (version)
    version[strcspn(version, "\r\n")] = 0;
  fclose(f);
  return 1;
}

/* ---------------------------------------------------------------- */

static int cmd_update(void)
{
  char url[400], err[160], path[220];
  struct mem idx = { 0, 0, 0 }, sig = { 0, 0, 0 };
  uint8_t s[64];
  FILE *f;

  need_repo();
  snprintf(url, sizeof(url), "%sINDEX.TXT", repo);
  if (http_get(url, to_mem, &idx, 0, err, sizeof(err)))
    die("%s", err);
  snprintf(url, sizeof(url), "%sINDEX.SIG", repo);
  if (http_get(url, to_mem, &sig, 0, err, sizeof(err)))
    die("%s", err);
  if (sig.len < 128 || !hex(sig.d, s, 64) ||
      crypto_ed25519_check(s, repo_key, (const uint8_t *)idx.d, idx.len))
    die("%s", "the package list's signature does NOT fit the repository key: not used");
  make_dirs(dbdir);
  mkdir(dbdir, 0755);
  join(path, sizeof(path), dbdir, "INDEX.TXT");
  f = fopen(path, "wb");
  if (!f || fwrite(idx.d, 1, idx.len, f) != idx.len)
    die("cannot write %s", path);
  fclose(f);
  printf("Package list updated: %d packages (signature good).\n", parse_index(idx.d));
  return 0;
}

static int cmd_list(void)
{
  int i;

  load_index();
  for (i = 0; i < npkgs; i++)
  {
    char v[24];
    int inst = installed(pkgs[i].name, v, sizeof(v));
    printf("%s %-10s %-8s %6ld KB  %s%s%s%s\n", inst ? "[*]" : "[ ]", pkgs[i].name, pkgs[i].version,
           (pkgs[i].size + 1023) / 1024, pkgs[i].desc, inst && strcmp(v, pkgs[i].version) ? "  (installed: " : "",
           inst && strcmp(v, pkgs[i].version) ? v : "", inst && strcmp(v, pkgs[i].version) ? ")" : "");
  }
  if (!npkgs)
    printf("The repository has no packages.\n");
  return 0;
}

/* a file name from an archive: relative, no "..", no drive */
static int safe_name(const char *n)
{
  if (!*n || *n == '/' || *n == '\\' || strchr(n, ':') || strstr(n, ".."))
    return 0;
  return 1;
}

static long octal(const char *p, int n)
{
  long v = 0;

  while (n-- > 0 && *p == ' ')
    p++;
  for (; *p >= '0' && *p <= '7'; p++)
    v = v * 8 + (*p - '0');
  return v;
}

/* unpack a tar archive below root; the files are listed in `lst` */
static int untar(const char *tar, FILE *lst)
{
  FILE *f = fopen(tar, "rb"), *out;
  unsigned char h[512], buf[4096];
  char name[300], path[420], *c;
  long size, n;
  int files = 0;

  if (!f)
    return -1;
  while (fread(h, 1, 512, f) == 512 && h[0])
  {
    if (h[345])
      snprintf(name, sizeof(name), "%.155s/%.100s", h + 345, h);
    else
      snprintf(name, sizeof(name), "%.100s", h);
    size = octal((char *)h + 124, 12);
    c = name;
    if (!strncmp(c, "./", 2))
      c += 2;
    if (h[156] == '5' || (*c && c[strlen(c) - 1] == '/'))      /* a directory */
    {
      if (*c && safe_name(c))
      {
        join(path, sizeof(path), root, c);
        make_dirs(path);
        mkdir(path, 0755);
      }
      continue;
    }
    if ((h[156] != '0' && h[156] != 0) || !safe_name(c))
    {
      fseek(f, (size + 511) / 512 * 512, SEEK_CUR);     /* links etc.: skipped */
      continue;
    }
    join(path, sizeof(path), root, c);
    for (c = path; *c; c++)
      if (*c == '/' && SEP == '\\')
        *c = '\\';
    make_dirs(path);
    out = fopen(path, "wb");
    if (!out)
    {
      fprintf(stderr, "PKG: cannot write %s\n", path);
      fclose(f);
      return -1;
    }
    for (n = size; n > 0; n -= 512)
    {
      if (fread(buf, 1, 512, f) != 512)
        break;
      fwrite(buf, 1, n > 512 ? 512 : (size_t)n, out);
    }
    fclose(out);
    fprintf(lst, "%s\n", path);
    printf("  %s\n", path);
    files++;
  }
  fclose(f);
  return files;
}

static int cmd_install(const char *name)
{
  char url[400], err[160], tmp[220], lst[220], shahex[65];
  struct pkg *p;
  struct dl d;
  uint8_t sum[32];
  FILE *l;
  int i, files;

  load_index();
  if (!(p = find(name)))
    die("no package \"%s\" in the list (PKG LIST, PKG UPDATE)", name);
  need_repo();
  make_dirs(dbdir);
  mkdir(dbdir, 0755);
  join(tmp, sizeof(tmp), dbdir, "DOWNLOAD.TMP");
  memset(&d, 0, sizeof(d));
  if (!(d.f = fopen(tmp, "wb")))
    die("cannot write %s", tmp);
  sha256_init(&d.sha);
  snprintf(url, sizeof(url), "%s%s", repo, p->file);
  printf("Downloading %s %s (%ld KB)...\n", p->name, p->version, (p->size + 1023) / 1024);
  i = http_get(url, to_file, &d, 0, err, sizeof(err));
  fclose(d.f);
  if (i)
  {
    remove(tmp);
    die("%s", err);
  }
  sha256_final(&d.sha, sum);
  for (i = 0; i < 32; i++)
    sprintf(shahex + i * 2, "%02x", sum[i]);
  if (strcmp(shahex, p->sha) || d.got != p->size)
  {
    remove(tmp);
    die("%s: the download does NOT match the package list (SHA-256): not installed", p->name);
  }
  list_path(lst, sizeof(lst), p->name);
  if (!(l = fopen(lst, "w")))
    die("cannot write %s", lst);
  fprintf(l, "%s\n", p->version);
  files = untar(tmp, l);
  fclose(l);
  remove(tmp);
  if (files < 0)
  {
    remove(lst);
    die("%s: could not be unpacked", p->name);
  }
  printf("Installed %s %s: %d files.\n", p->name, p->version, files);
  return 0;
}

static int cmd_remove(const char *name)
{
  char lst[220], line[420];
  FILE *l;
  int n = 0, first = 1;

  list_path(lst, sizeof(lst), name);
  if (!(l = fopen(lst, "r")))
    die("%s is not installed", name);
  while (fgets(line, sizeof(line), l))
  {
    line[strcspn(line, "\r\n")] = 0;
    if (first)
    {
      first = 0;                        /* the version */
      continue;
    }
    if (*line && !remove(line))
      n++;
  }
  fclose(l);
  remove(lst);
  printf("Removed %s: %d files.\n", name, n);
  return 0;
}

int main(int argc, char **argv)
{
  config(argv[0]);
  if (argc >= 2 && !strcasecmp(argv[1], "UPDATE"))
    return cmd_update();
  if (argc >= 2 && !strcasecmp(argv[1], "LIST"))
    return cmd_list();
  if (argc >= 3 && !strcasecmp(argv[1], "INSTALL"))
    return cmd_install(argv[2]);
  if (argc >= 3 && !strcasecmp(argv[1], "REMOVE"))
    return cmd_remove(argv[2]);
  if (argc >= 2 && !strcasecmp(argv[1], "INFO"))
  {
    printf("Repository: %s\nKey: %s\nPackages go to: %s\nPackage records: %s\n", repo[0] ? repo : "(none)",
           have_key ? "set" : "(none)", root, dbdir);
    return 0;
  }
  printf("modernDOS package manager\n"
         "  PKG UPDATE          fetch the repository's package list\n"
         "  PKG LIST            packages, [*] = installed\n"
         "  PKG INSTALL name    download, check and unpack\n"
         "  PKG REMOVE name     delete an installed package\n"
         "  PKG INFO            repository and directories\n"
         "Repository and key: %sPKG.CFG\n", cfgdir);
  return 2;
}
