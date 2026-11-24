/*
 * SSHD - SSH server for modernDOS
 *
 *   SSHD [/P port]        run the server (default port 22)
 *   SSHD /PASSWD          set the login password (stored hashed)
 *   SSHD /NOPASSWD        remove it (public keys only)
 *
 * Files, in the directory of SSHD.EXE (or in %SSHDIR%):
 *   HOSTKEY.ED    the host's ed25519 key (made on the first start)
 *   AUTHKEYS.TXT  public keys allowed to log in, OpenSSH authorized_keys
 *                 format ("ssh-ed25519 AAAA... comment"), one per line
 *   PASSWD.TXT    password hash (Argon2i), if a password is set
 *   SEED.BIN      random seed kept between runs
 * Without AUTHKEYS.TXT and PASSWD.TXT nobody can log in.
 *
 * Sessions: "shell" runs COMMAND.COM in an MDVMM job slot (SSHSLOT.COM)
 * whose screen is sent to the client as ANSI text; "exec" runs one command
 * in a slot, its output redirected to a file that is sent back; the "sftp"
 * subsystem serves files (scp and sftp). Press Esc on the DOS console to
 * stop the server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "crypto/monocypher.h"
#include "crypto/monocypher-ed25519.h"
#include "buf.h"
#include "rng.h"
#include "ssh.h"
#include "plat.h"
#include "console.h"
#include "sftp.h"

#define MAX_CONN       4
#define MAX_CHAN       2
#define MAX_KEYS       32
#define WINDOW         (256 * 1024)
#define MAX_PKT        32768
#define AUTH_TRIES     6
#define AUTH_TIMEOUT   120000           /* ms to log in */
#define SCREEN_MS      40               /* screen polling interval */

enum { CH_FREE, CH_OPEN, CH_SHELL, CH_EXEC, CH_SFTP };

struct chan {
  int type;
  uint32_t peer, peer_window, peer_max;
  uint32_t window;                      /* what the client may still send */
  int pty;
  int slot;                             /* job slot, or -1 */
  struct buf inq, outq;
  int got_eof, sent_eof, sent_close, got_close;
  int done;                             /* the job ended */
  int code;
  struct console con;
  uint32_t last_screen;
  char outfile[96];
  struct sftp *sftp;
};

struct conn {
  int used;
  struct ssh s;
  char peer[32];
  int authed;
  char user[64];
  int tries;
  uint32_t start;
  struct chan ch[MAX_CHAN];
};

static struct conn conns[MAX_CONN];
static uint8_t host_sk[64], host_pk[32];
static uint8_t auth_keys[MAX_KEYS][32];
static int nkeys;
static char pw_line[256];               /* "argon2i$blocks$passes$salt$hash" */
static char dir[96];                    /* where SSHD.EXE and its files are */
static int orphan[8];                   /* slots of closed sessions to recover */

static void path_of(char *out, size_t n, const char *file)
{
  snprintf(out, n, "%s%s", dir, file);
}

/* ---------------------------------------------------------------- */
/* keys and passwords                                               */

static int load_hostkey(void)
{
  char path[128], fp[64];
  uint8_t seed[32];
  FILE *f;

  path_of(path, sizeof(path), "HOSTKEY.ED");
  f = fopen(path, "rb");
  if (f)
  {
    size_t n = fread(seed, 1, 32, f);
    fclose(f);
    if (n != 32)
    {
      plat_log("%s is damaged\n", path);
      return 0;
    }
  }
  else
  {
    plat_log("making a new host key %s\n", path);
    rng_bytes(seed, 32);
    f = fopen(path, "wb");
    if (!f || fwrite(seed, 1, 32, f) != 32)
    {
      plat_log("cannot write %s\n", path);
      if (f)
        fclose(f);
      return 0;
    }
    fclose(f);
  }
  crypto_ed25519_key_pair(host_sk, host_pk, seed);     /* wipes seed */
  ssh_fingerprint(host_pk, fp);
  plat_log("host key ED25519 %s\n", fp);
  return 1;
}

static void load_authkeys(void)
{
  char path[128], line[1024];
  FILE *f;

  nkeys = 0;
  path_of(path, sizeof(path), "AUTHKEYS.TXT");
  f = fopen(path, "r");
  if (!f)
    return;
  while (fgets(line, sizeof(line), f) && nkeys < MAX_KEYS)
  {
    char *p = line, *b64, *end;
    uint8_t blob[128];
    int n;
    struct buf b;
    uint32_t an, kn;
    const uint8_t *alg, *pk;

    while (*p == ' ' || *p == '\t')
      p++;
    if (strncmp(p, "ssh-ed25519 ", 12))
      continue;                         /* comments, other key types */
    b64 = p + 12;
    while (*b64 == ' ')
      b64++;
    end = b64;
    while (*end && !isspace((unsigned char)*end))
      end++;
    n = b64_decode(b64, (size_t)(end - b64), blob, sizeof(blob));
    if (n <= 0)
      continue;
    b.d = blob; b.len = (size_t)n; b.cap = 0; b.pos = 0; b.err = 0;
    alg = get_str(&b, &an);
    pk = get_str(&b, &kn);
    if (b.err || an != 11 || memcmp(alg, "ssh-ed25519", 11) || kn != 32)
      continue;
    memcpy(auth_keys[nkeys++], pk, 32);
  }
  fclose(f);
}

static void load_password(void)
{
  char path[128];
  FILE *f;

  pw_line[0] = 0;
  path_of(path, sizeof(path), "PASSWD.TXT");
  f = fopen(path, "r");
  if (!f)
    return;
  if (!fgets(pw_line, sizeof(pw_line), f))
    pw_line[0] = 0;
  pw_line[strcspn(pw_line, "\r\n")] = 0;
  fclose(f);
  if (strncmp(pw_line, "argon2i$", 8))
    pw_line[0] = 0;
}

static int hexval(const char *s, uint8_t *out, int n)
{
  int i;

  for (i = 0; i < n; i++)
  {
    unsigned v;
    if (sscanf(s + i * 2, "%2x", &v) != 1)
      return 0;
    out[i] = (uint8_t)v;
  }
  return 1;
}

static void argon(uint8_t hash[32], const char *pw, size_t pwlen, const uint8_t salt[16],
                  uint32_t blocks, uint32_t passes)
{
  crypto_argon2_config cfg;
  crypto_argon2_inputs in;
  void *work = malloc((size_t)blocks * 1024);

  if (!work)
  {
    memset(hash, 0, 32);
    return;
  }
  cfg.algorithm = CRYPTO_ARGON2_I;
  cfg.nb_blocks = blocks;
  cfg.nb_passes = passes;
  cfg.nb_lanes = 1;
  in.pass = (const uint8_t *)pw;
  in.pass_size = (uint32_t)pwlen;
  in.salt = salt;
  in.salt_size = 16;
  crypto_argon2(hash, 32, work, cfg, in, crypto_argon2_no_extras);
  free(work);
}

static int check_password(const char *pw, size_t len)
{
  uint8_t salt[16], want[32], got[32];
  unsigned blocks, passes;
  char saltx[40], hashx[80];

  if (!pw_line[0] || sscanf(pw_line, "argon2i$%u$%u$%32[0-9a-f]$%64[0-9a-f]", &blocks, &passes,
                            saltx, hashx) != 4 ||
      blocks < 8 || blocks > 65536 || passes < 1 || passes > 16 ||
      !hexval(saltx, salt, 16) || !hexval(hashx, want, 32))
    return 0;
  argon(got, pw, len, salt, blocks, passes);
  return !crypto_verify32(got, want);
}

static void read_secret(const char *prompt, char *out, size_t n)
{
  size_t len = 0;
  int c;

  fputs(prompt, stdout);
  fflush(stdout);
  for (;;)
  {
    while ((c = plat_key()) < 0)
      plat_idle();
    if (c == '\r' || c == '\n')
      break;
    if ((c == 8 || c == 127) && len)
      len--;
    else if (c >= 32 && len + 1 < n)
      out[len++] = (char)c;
  }
  out[len] = 0;
  fputs("\n", stdout);
}

static int set_password(void)
{
  char a[128], b[128], path[128];
  uint8_t salt[16], hash[32];
  FILE *f;
  int i;

  read_secret("New password: ", a, sizeof(a));
  read_secret("Again: ", b, sizeof(b));
  if (strcmp(a, b) || strlen(a) < 8)
  {
    printf(strcmp(a, b) ? "The passwords differ.\n" : "Use at least 8 characters.\n");
    return 1;
  }
  rng_bytes(salt, sizeof(salt));
  argon(hash, a, strlen(a), salt, 4096, 3);
  crypto_wipe(a, sizeof(a));
  crypto_wipe(b, sizeof(b));
  path_of(path, sizeof(path), "PASSWD.TXT");
  f = fopen(path, "w");
  if (!f)
  {
    printf("Cannot write %s\n", path);
    return 1;
  }
  fprintf(f, "argon2i$4096$3$");
  for (i = 0; i < 16; i++)
    fprintf(f, "%02x", salt[i]);
  fprintf(f, "$");
  for (i = 0; i < 32; i++)
    fprintf(f, "%02x", hash[i]);
  fprintf(f, "\n");
  fclose(f);
  printf("Password set (%s).\n", path);
  return 0;
}

/* ---------------------------------------------------------------- */
/* authentication                                                   */

static void auth_failure(struct conn *c)
{
  struct buf b;

  buf_init(&b);
  put_u8(&b, SSH_MSG_USERAUTH_FAILURE);
  put_cstr(&b, pw_line[0] ? "publickey,password" : "publickey");
  put_u8(&b, 0);
  ssh_send(&c->s, &b);
  buf_free(&b);
}

static int key_allowed(const uint8_t pk[32])
{
  int i;

  load_authkeys();                      /* edits apply without a restart */
  for (i = 0; i < nkeys; i++)
    if (!crypto_verify32(auth_keys[i], pk))
      return 1;
  return 0;
}

static void userauth(struct conn *c, struct buf *p)
{
  char user[64], service[32], method[32];
  struct buf b;

  buf_init(&b);
  get_cstr(p, user, sizeof(user));
  get_cstr(p, service, sizeof(service));
  get_cstr(p, method, sizeof(method));
  if (p->err || strcmp(service, "ssh-connection"))
  {
    ssh_disconnect(&c->s, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE, "bad service");
    return;
  }
  if (!strcmp(method, "publickey"))
  {
    int hassig = get_u8(p);
    uint32_t an, bn, sn, n2, kn;
    const uint8_t *alg = get_str(p, &an), *blob = get_str(p, &bn), *sig = NULL;
    struct buf kb;
    const uint8_t *pk;
    if (hassig)
      sig = get_str(p, &sn);
    kb.d = (uint8_t *)blob; kb.len = bn; kb.cap = 0; kb.pos = 0; kb.err = 0;
    get_str(&kb, &n2);
    pk = get_str(&kb, &kn);
    if (p->err || kb.err || an != 11 || memcmp(alg, "ssh-ed25519", 11) || kn != 32 ||
        !key_allowed(pk))
    {
      if (hassig)
        c->tries++;
      auth_failure(c);
    }
    else if (!hassig)
    {
      put_u8(&b, SSH_MSG_USERAUTH_PK_OK);
      put_str(&b, alg, an);
      put_str(&b, blob, bn);
      ssh_send(&c->s, &b);
    }
    else
    {
      struct buf sb, data;
      uint32_t sa, sl;
      const uint8_t *s64;
      sb.d = (uint8_t *)sig; sb.len = sn; sb.cap = 0; sb.pos = 0; sb.err = 0;
      get_str(&sb, &sa);
      s64 = get_str(&sb, &sl);
      buf_init(&data);
      put_str(&data, c->s.sid, 32);
      put_u8(&data, SSH_MSG_USERAUTH_REQUEST);
      put_cstr(&data, user);
      put_cstr(&data, service);
      put_cstr(&data, "publickey");
      put_u8(&data, 1);
      put_str(&data, alg, an);
      put_str(&data, blob, bn);
      if (!sb.err && sl == 64 && !crypto_ed25519_check(s64, pk, data.d, data.len))
      {
        char fp[64];
        ssh_fingerprint(pk, fp);
        plat_log("%s: user %s logged in with key %s\n", c->peer, user, fp);
        c->authed = 1;
        strcpy(c->user, user);
        put_u8(&b, SSH_MSG_USERAUTH_SUCCESS);
        ssh_send(&c->s, &b);
      }
      else
      {
        c->tries++;
        auth_failure(c);
      }
      buf_free(&data);
    }
  }
  else if (!strcmp(method, "password") && pw_line[0])
  {
    uint32_t n;
    const uint8_t *pw;
    get_u8(p);
    pw = get_str(p, &n);
    if (pw && n < 128 && check_password((const char *)pw, n))
    {
      plat_log("%s: user %s logged in with the password\n", c->peer, user);
      c->authed = 1;
      strcpy(c->user, user);
      put_u8(&b, SSH_MSG_USERAUTH_SUCCESS);
      ssh_send(&c->s, &b);
    }
    else
    {
      plat_log("%s: wrong password for %s\n", c->peer, user);
      c->tries++;
      auth_failure(c);
    }
  }
  else
    auth_failure(c);
  buf_free(&b);
  if (c->tries >= AUTH_TRIES)
    ssh_disconnect(&c->s, SSH_DISCONNECT_NO_MORE_AUTH_METHODS, "too many failed logins");
}

/* ---------------------------------------------------------------- */
/* channels                                                         */

static struct chan *chan_get(struct conn *c, struct buf *p)
{
  uint32_t n = get_u32(p);

  if (p->err || n >= MAX_CHAN || c->ch[n].type == CH_FREE)
  {
    ssh_disconnect(&c->s, SSH_DISCONNECT_PROTOCOL_ERROR, "bad channel");
    return NULL;
  }
  return &c->ch[n];
}

static void chan_reply(struct conn *c, struct chan *ch, int ok)
{
  struct buf b;

  buf_init(&b);
  put_u8(&b, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
  put_u32(&b, ch->peer);
  ssh_send(&c->s, &b);
  buf_free(&b);
}

static void chan_free(struct chan *ch)
{
  if (ch->slot >= 0 && !ch->done)
  {
    int i;
    for (i = 0; i < 8; i++)
      if (!orphan[i])
      {
        orphan[i] = ch->slot;           /* end it in the background */
        break;
      }
  }
  if (ch->outfile[0])
    remove(ch->outfile);
  buf_free(&ch->inq);
  buf_free(&ch->outq);
  sftp_free(ch->sftp);
  memset(ch, 0, sizeof(*ch));
  ch->slot = -1;
}

static void chan_open(struct conn *c, struct buf *p)
{
  char type[32];
  uint32_t peer, win, max;
  struct buf b;
  int i;

  get_cstr(p, type, sizeof(type));
  peer = get_u32(p);
  win = get_u32(p);
  max = get_u32(p);
  buf_init(&b);
  for (i = 0; i < MAX_CHAN && c->ch[i].type != CH_FREE; i++)
    ;
  if (p->err || strcmp(type, "session") || i == MAX_CHAN)
  {
    put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
    put_u32(&b, peer);
    put_u32(&b, strcmp(type, "session") ? 3 : 4);  /* unknown type / no resources */
    put_cstr(&b, strcmp(type, "session") ? "only sessions" : "too many sessions");
    put_cstr(&b, "");
    ssh_send(&c->s, &b);
    buf_free(&b);
    return;
  }
  memset(&c->ch[i], 0, sizeof(c->ch[i]));
  c->ch[i].type = CH_OPEN;
  c->ch[i].peer = peer;
  c->ch[i].peer_window = win;
  c->ch[i].peer_max = max < 1024 ? 1024 : max;
  c->ch[i].window = WINDOW;
  c->ch[i].slot = -1;
  buf_init(&c->ch[i].inq);
  buf_init(&c->ch[i].outq);
  put_u8(&b, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
  put_u32(&b, peer);
  put_u32(&b, (uint32_t)i);
  put_u32(&b, WINDOW);
  put_u32(&b, MAX_PKT);
  ssh_send(&c->s, &b);
  buf_free(&b);
}

static int start_job(struct conn *c, struct chan *ch, const char *cmd)
{
  ch->slot = plat_job_start(cmd);
  if (ch->slot < 0)
  {
    plat_log("%s: no free job slot (SSHSLOT loaded? sessions busy?)\n", c->peer);
    return 0;
  }
  return 1;
}

static void chan_request(struct conn *c, struct buf *p)
{
  struct chan *ch = chan_get(c, p);
  char req[32];
  int want, ok = 0;

  if (!ch)
    return;
  get_cstr(p, req, sizeof(req));
  want = get_u8(p);
  if (!strcmp(req, "pty-req"))
  {
    ch->pty = 1;
    ok = 1;
  }
  else if (!strcmp(req, "shell") && ch->type == CH_OPEN)
  {
    if (start_job(c, ch, ""))
    {
      ch->type = CH_SHELL;
      con_init(&ch->con);
      plat_log("%s: shell in session %d\n", c->peer, ch->slot);
      ok = 1;
    }
  }
  else if (!strcmp(req, "exec") && ch->type == CH_OPEN)
  {
    char cmd[200], line[400];
    static int seq;
    if (get_cstr(p, cmd, sizeof(cmd)) && cmd[0])
    {
      snprintf(ch->outfile, sizeof(ch->outfile), "%sSSH%05d.OUT", plat_tempdir(),
               (seq++ + (int)plat_ms()) % 100000);
#ifdef __DJGPP__
      /* through a second shell: its output handle is inherited by all it
         runs, also the commands of a batch file (a redirected batch file
         alone does not pass it on). No space before ">": ECHO would
         write it. */
      snprintf(line, sizeof(line), "%%COMSPEC%% /C %s>%s", cmd, ch->outfile);
#else
      snprintf(line, sizeof(line), "%s > %s", cmd, ch->outfile);   /* sh: "3>" is fd 3 */
#endif
      if (strlen(line) < 118 && start_job(c, ch, line))
      {
        ch->type = CH_EXEC;
        plat_log("%s: exec \"%s\" in session %d\n", c->peer, cmd, ch->slot);
        ok = 1;
      }
      else
        ch->outfile[0] = 0;
    }
  }
  else if (!strcmp(req, "subsystem") && ch->type == CH_OPEN)
  {
    char name[32];
    if (get_cstr(p, name, sizeof(name)) && !strcmp(name, "sftp"))
    {
      ch->sftp = sftp_new();
      ch->type = CH_SFTP;
      plat_log("%s: sftp\n", c->peer);
      ok = 1;
    }
  }
  else if (!strcmp(req, "window-change"))
    ok = 1;
  else if ((!strcmp(req, "signal") || !strcmp(req, "break")) && ch->type == CH_SHELL)
  {
    uint16_t k = 0x2e03;                /* Ctrl+C */
    plat_keys(ch->slot, &k, 1);
    ok = 1;
  }
  if (want)
    chan_reply(c, ch, ok);
}

static void chan_data(struct conn *c, struct buf *p)
{
  struct chan *ch = chan_get(c, p);
  uint32_t n;
  const uint8_t *d;

  if (!ch)
    return;
  d = get_str(p, &n);
  if (!d || n > ch->window)
  {
    ssh_disconnect(&c->s, SSH_DISCONNECT_PROTOCOL_ERROR, "window exceeded");
    return;
  }
  ch->window -= n;
  if (ch->type == CH_SHELL || ch->type == CH_SFTP)
    put_bytes(&ch->inq, d, n);
}

static void handle(struct conn *c, struct buf *p)
{
  uint8_t t = p->d[0];
  struct buf b;
  struct chan *ch;

  buf_init(&b);
  if (t == SSH_MSG_SERVICE_REQUEST)
  {
    char svc[32];
    get_cstr(p, svc, sizeof(svc));
    if (strcmp(svc, "ssh-userauth"))
      ssh_disconnect(&c->s, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE, "unknown service");
    else
    {
      put_u8(&b, SSH_MSG_SERVICE_ACCEPT);
      put_cstr(&b, svc);
      ssh_send(&c->s, &b);
    }
  }
  else if (t == SSH_MSG_USERAUTH_REQUEST)
  {
    if (c->authed)
      ;                                 /* ignore after success (RFC 4252) */
    else
      userauth(c, p);
  }
  else if (!c->authed)
    ssh_disconnect(&c->s, SSH_DISCONNECT_PROTOCOL_ERROR, "not logged in");
  else
    switch (t)
    {
      case SSH_MSG_GLOBAL_REQUEST:
      {
        char name[64];
        get_cstr(p, name, sizeof(name));
        if (get_u8(p))
        {
          put_u8(&b, SSH_MSG_REQUEST_FAILURE);
          ssh_send(&c->s, &b);
        }
        break;
      }
      case SSH_MSG_CHANNEL_OPEN: chan_open(c, p); break;
      case SSH_MSG_CHANNEL_REQUEST: chan_request(c, p); break;
      case SSH_MSG_CHANNEL_DATA: chan_data(c, p); break;
      case SSH_MSG_CHANNEL_EXTENDED_DATA: break;
      case SSH_MSG_CHANNEL_WINDOW_ADJUST:
        if ((ch = chan_get(c, p)))
          ch->peer_window += get_u32(p);
        break;
      case SSH_MSG_CHANNEL_EOF:
        if ((ch = chan_get(c, p)))
          ch->got_eof = 1;
        break;
      case SSH_MSG_CHANNEL_CLOSE:
        if ((ch = chan_get(c, p)))
          ch->got_close = 1;
        break;
      case SSH_MSG_CHANNEL_SUCCESS:
      case SSH_MSG_CHANNEL_FAILURE:
      case SSH_MSG_REQUEST_SUCCESS:
      case SSH_MSG_REQUEST_FAILURE:
        break;
      default:
      {
        put_u8(&b, SSH_MSG_UNIMPLEMENTED);
        put_u32(&b, c->s.seq_in - 1);
        ssh_send(&c->s, &b);
      }
    }
  buf_free(&b);
}

/* ---------------------------------------------------------------- */
/* channel work                                                     */

static void send_exit(struct conn *c, struct chan *ch)
{
  struct buf b;

  buf_init(&b);
  put_u8(&b, SSH_MSG_CHANNEL_REQUEST);
  put_u32(&b, ch->peer);
  put_cstr(&b, "exit-status");
  put_u8(&b, 0);
  put_u32(&b, (uint32_t)ch->code);
  ssh_send(&c->s, &b);
  buf_free(&b);
}

static void send_close(struct conn *c, struct chan *ch)
{
  struct buf b;

  buf_init(&b);
  if (!ch->sent_eof)
  {
    put_u8(&b, SSH_MSG_CHANNEL_EOF);
    put_u32(&b, ch->peer);
    ssh_send(&c->s, &b);
    ch->sent_eof = 1;
  }
  put_u8(&b, SSH_MSG_CHANNEL_CLOSE);
  put_u32(&b, ch->peer);
  ssh_send(&c->s, &b);
  ch->sent_close = 1;
  buf_free(&b);
}

/* outq -> CHANNEL_DATA within the client's window */
static int pump_out(struct conn *c, struct chan *ch)
{
  struct buf b;
  int moved = 0;

  buf_init(&b);
  while (ch->outq.len && ch->peer_window && ssh_pending(&c->s) < 64 * 1024)
  {
    uint32_t n = (uint32_t)ch->outq.len;
    if (n > ch->peer_window)
      n = ch->peer_window;
    if (n > ch->peer_max)
      n = ch->peer_max;
    if (n > 16384)
      n = 16384;
    put_u8(&b, SSH_MSG_CHANNEL_DATA);
    put_u32(&b, ch->peer);
    put_str(&b, ch->outq.d, n);
    ssh_send(&c->s, &b);
    buf_consume(&ch->outq, n);
    ch->peer_window -= n;
    moved = 1;
  }
  buf_free(&b);
  /* let the client send more */
  if (ch->window < WINDOW / 2 && ch->inq.len < WINDOW / 2)
  {
    buf_init(&b);
    put_u8(&b, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    put_u32(&b, ch->peer);
    put_u32(&b, WINDOW - ch->window);
    ssh_send(&c->s, &b);
    buf_free(&b);
    ch->window = WINDOW;
  }
  return moved;
}

static void shell_work(struct conn *c, struct chan *ch)
{
  uint8_t cells[4000];
  int row, col, hidden, state;

  (void)c;
  /* keys from the client */
  if (ch->inq.len && !ch->done)
  {
    uint16_t keys[64];
    int n = con_keys(&ch->con, ch->inq.d, ch->inq.len, keys, 15), taken;
    taken = n ? plat_keys(ch->slot, keys, n) : 0;
    if (taken == n)
      buf_consume(&ch->inq, ch->con.used);
    else if (taken > 0)
    {
      /* the BIOS buffer took only some: parse again next time */
      struct console tmp = ch->con;
      con_keys(&tmp, ch->inq.d, ch->inq.len, keys, taken);
      buf_consume(&ch->inq, tmp.used);
      ch->con.last_cr = tmp.last_cr;
    }
  }
  /* the screen */
  if (!ch->done && !ch->outq.len && plat_ms() - ch->last_screen >= SCREEN_MS &&
      ch->peer_window >= 32768)
  {
    ch->last_screen = plat_ms();
    if (plat_screen(ch->slot, cells, &row, &col, &hidden))
      con_render(&ch->con, cells, row, col, hidden, &ch->outq);
  }
  if (!ch->done)
  {
    state = plat_job_state(ch->slot, &ch->code);
    if (state == JOB_DONE || state < 0)
    {
      ch->done = 1;
      ch->slot = -1;
      con_reset(&ch->outq);
    }
  }
}

static void exec_work(struct conn *c, struct chan *ch)
{
  int state;

  if (ch->done)
    return;
  state = plat_job_state(ch->slot, &ch->code);
  if (state == JOB_DONE || state < 0)
  {
    FILE *f = fopen(ch->outfile, "rb");
    ch->done = 1;
    ch->slot = -1;
    if (f)
    {
      char tmp[4096];
      size_t n;
      while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
        put_bytes(&ch->outq, tmp, n);
      fclose(f);
    }
    remove(ch->outfile);
    ch->outfile[0] = 0;
    plat_log("%s: exec finished, exit code %d\n", c->peer, ch->code);
  }
}

static int conn_work(struct conn *c)
{
  int i, busy = 0;

  for (i = 0; i < MAX_CHAN; i++)
  {
    struct chan *ch = &c->ch[i];
    if (ch->type == CH_FREE)
      continue;
    if (ch->got_close)
    {
      if (!ch->sent_close)
        send_close(c, ch);
      chan_free(ch);
      continue;
    }
    if (!ssh_ready(&c->s))
      continue;
    if (ch->type == CH_SHELL)
      shell_work(c, ch);
    else if (ch->type == CH_EXEC)
      exec_work(c, ch);
    else if (ch->type == CH_SFTP && ch->outq.len < 128 * 1024 && ch->inq.len)
    {
      if (sftp_input(ch->sftp, &ch->inq, &ch->outq) < 0)
        ssh_disconnect(&c->s, SSH_DISCONNECT_PROTOCOL_ERROR, "bad sftp packet");
      busy = 1;
    }
    if (ch->type == CH_SFTP && ch->got_eof && !ch->inq.len && !ch->outq.len && !ch->sent_close)
    {
      ch->code = 0;
      send_exit(c, ch);                 /* scp checks it */
      send_close(c, ch);
    }
    busy |= pump_out(c, ch);
    if ((ch->type == CH_SHELL || ch->type == CH_EXEC) && ch->done && !ch->outq.len &&
        !ch->sent_close)
    {
      send_exit(c, ch);
      send_close(c, ch);
    }
  }
  return busy;
}

static void conn_close(struct conn *c)
{
  int i;

  plat_log("%s: closed (%s)\n", c->peer, c->s.why[0] ? c->s.why : "done");
  ssh_io(&c->s);                        /* a last DISCONNECT */
  plat_close(c->s.fd);
  for (i = 0; i < MAX_CHAN; i++)
    if (c->ch[i].type != CH_FREE)
      chan_free(&c->ch[i]);
  ssh_free(&c->s);
  c->used = 0;
}

/* sessions of closed connections: Ctrl+C and EXIT until the shell ends */
static void orphans(void)
{
  static uint32_t last;
  int i, code;

  if (plat_ms() - last < 2000)
    return;
  last = plat_ms();
  for (i = 0; i < 8; i++)
    if (orphan[i])
    {
      int st = plat_job_state(orphan[i], &code);
      if (st == JOB_DONE || st == JOB_NONE || st < 0)
        orphan[i] = 0;
      else
      {
        static const uint16_t keys[] = { 0x2e03, 0x1c0d, 0x1265, 0x2d78, 0x1769, 0x1474, 0x1c0d };
        plat_keys(orphan[i], keys, 7);  /* ^C Enter "exit" Enter */
      }
    }
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
  int port = 22, lfd, i;
  char seed[128], *slash;

  snprintf(dir, sizeof(dir), "%s", argv[0]);
  for (i = 0; dir[i]; i++)
    if (dir[i] == '\\')
      dir[i] = '/';
  slash = strrchr(dir, '/');
  if (slash)
    slash[1] = 0;
  else
    dir[0] = 0;
  /* SSHDIR: keys and settings elsewhere (a disk that keeps them) */
  if (getenv("SSHDIR") && strlen(getenv("SSHDIR")) < sizeof(dir) - 2)
  {
    snprintf(dir, sizeof(dir), "%s", getenv("SSHDIR"));
    for (i = 0; dir[i]; i++)
      if (dir[i] == '\\')
        dir[i] = '/';
    if (i && dir[i - 1] != '/')
      strcat(dir, "/");
  }
  path_of(seed, sizeof(seed), "SEED.BIN");

  for (i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "/P") || !strcmp(argv[i], "/p") || !strcmp(argv[i], "-p"))
    {
      if (++i < argc)
        port = atoi(argv[i]);
    }
    else if (!strcasecmp(argv[i], "/PASSWD"))
    {
      rng_init(seed);
      return set_password();
    }
    else if (!strcasecmp(argv[i], "/NOPASSWD"))
    {
      char path[128];
      path_of(path, sizeof(path), "PASSWD.TXT");
      remove(path);
      printf("Password removed: public keys only.\n");
      return 0;
    }
    else
    {
      printf("Usage: SSHD [/P port] | /PASSWD | /NOPASSWD\n");
      return 2;
    }
  }

  plat_log("modernDOS SSH server\n");
  rng_init(seed);
  if (!plat_init() || !load_hostkey())
    return 1;
  load_authkeys();
  load_password();
  if (!nkeys && !pw_line[0])
    plat_log("warning: no AUTHKEYS.TXT and no password: nobody can log in\n");
  lfd = plat_listen(port);
  if (lfd < 0)
  {
    plat_log("cannot listen on port %d\n", port);
    return 1;
  }
  plat_log("listening on port %d (%d keys%s); Esc stops\n", port, nkeys,
           pw_line[0] ? ", password" : "");

  for (;;)
  {
    int busy = 0, fd;
    char peer[32];
    struct buf p;

    if (plat_key() == 27)
      break;
    fd = plat_accept(lfd, peer, sizeof(peer));
    if (fd >= 0)
    {
      for (i = 0; i < MAX_CONN && conns[i].used; i++)
        ;
      if (i == MAX_CONN)
      {
        plat_log("%s: refused, too many connections\n", peer);
        plat_close(fd);
      }
      else
      {
        struct conn *c = &conns[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        strcpy(c->peer, peer);
        c->start = plat_ms();
        c->s.host_sk = host_sk;
        c->s.host_pk = host_pk;
        ssh_start(&c->s, fd, 1);
        for (fd = 0; fd < MAX_CHAN; fd++)
          c->ch[fd].slot = -1;
        plat_log("%s: connected\n", peer);
      }
      busy = 1;
    }
    buf_init(&p);
    for (i = 0; i < MAX_CONN; i++)
    {
      struct conn *c = &conns[i];
      int r;
      if (!c->used)
        continue;
      busy |= ssh_io(&c->s);
      while ((r = ssh_packet(&c->s, &p)) > 0)
      {
        handle(c, &p);
        busy = 1;
      }
      if (!c->authed && plat_ms() - c->start > AUTH_TIMEOUT)
        ssh_disconnect(&c->s, SSH_DISCONNECT_AUTH_CANCELLED, "login timeout");
      if (!c->s.dead)
        busy |= conn_work(c);
      busy |= ssh_io(&c->s);
      if (c->s.dead)
        conn_close(c);
    }
    buf_free(&p);
    orphans();
    if (!busy)
      plat_idle();
  }
  for (i = 0; i < MAX_CONN; i++)
    if (conns[i].used)
    {
      ssh_disconnect(&conns[i].s, SSH_DISCONNECT_BY_APPLICATION, "server stopped");
      conn_close(&conns[i]);
    }
  plat_close(lfd);
  rng_save(seed);
  plat_log("stopped\n");
  return 0;
}
