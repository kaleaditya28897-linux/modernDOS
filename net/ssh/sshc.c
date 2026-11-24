/*
 * SSH - SSH client for modernDOS
 *
 *   SSH [options] [user@]host [command]
 *     /P port      port (default 22)
 *     /I keyfile   private key (default ID.KEY next to SSH.EXE)
 *     /L user      user name (default: the one before @, else "dos")
 *     /Y           accept an unknown host key without asking (saved)
 *     /T           a terminal even when a command is given
 *   SSH /KEYGEN    make ID.KEY (ed25519) and print the public key line
 *                  to add to the server's ~/.ssh/authorized_keys
 *
 * Without a command: an interactive session in an xterm-like terminal on
 * the DOS screen (type ~. at the start of a line to disconnect). With a
 * command: its output goes to standard output; the exit code is the
 * command's.
 * Known host keys are kept in KNOWNHST.TXT next to SSH.EXE; a changed host
 * key is refused. ID.KEY is an OpenSSH private key file without
 * passphrase, so an existing ed25519 key can be copied from another
 * machine too.
 */
#include <stdio.h>
#include <stdarg.h>
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
#include "term.h"
#include "keyfile.h"

#define WINDOW   (1024 * 1024)
#define MAX_PKT  32768

static char dir[96], host[128], user[64], cmd[512], keypath[128];
static int port = 22, accept_new, want_tty, have_key;
static uint8_t key_sk[64], key_pk[32];
static struct ssh s;
static struct term term;
static struct buf replies;

enum { A_SERVICE, A_KEY, A_NONE, A_PASSWORD, A_KBDINT, A_DONE };
static int auth_state, pw_tries, service_sent;
static uint32_t peer_chan, peer_window, peer_max;
static int chan_open, chan_closed, exit_code = 255, got_exit;

#ifdef __DJGPP__
/* remote commands are not for local wildcard expansion */
char **__crt0_glob_function(char *arg)
{
  (void)arg;
  return 0;
}
#endif

static void path_of(char *out, size_t n, const char *file)
{
  snprintf(out, n, "%s%s", dir, file);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
  char b[400];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  fputs(b, stderr);
}

static void read_line(const char *prompt, char *out, size_t n, int echo)
{
  size_t len = 0;
  int c;

  fputs(prompt, stderr);
  fflush(stderr);
  for (;;)
  {
    while ((c = plat_key()) < 0)
    {
      ssh_io(&s);                       /* keep the connection alive */
      plat_idle();
    }
    if (c == '\r' || c == '\n')
      break;
    if ((c == 8 || c == 127) && len)
    {
      len--;
      if (echo)
        fputs("\b \b", stderr);
    }
    else if (c >= 32 && len + 1 < n)
    {
      out[len++] = (char)c;
      if (echo)
        fputc(c, stderr);
    }
  }
  out[len] = 0;
  fputs("\n", stderr);
}

/* ---------------------------------------------------------------- */
/* known hosts: "host port ssh-ed25519 BASE64" lines                */

static int check_hostkey(struct ssh *ss, const uint8_t pk[32])
{
  char path[128], line[512], fp[64], want[128], answer[8];
  struct buf b;
  FILE *f;
  int found = 0, same = 0;

  (void)ss;
  buf_init(&b);
  ssh_ed25519_blob(&b, pk);
  b64_encode(b.d, b.len, want, 1);
  buf_free(&b);
  ssh_fingerprint(pk, fp);
  path_of(path, sizeof(path), "KNOWNHST.TXT");
  f = fopen(path, "r");
  if (f)
  {
    while (fgets(line, sizeof(line), f))
    {
      char h[128], alg[32], key[128];
      int p;
      if (sscanf(line, "%127s %d %31s %127s", h, &p, alg, key) != 4)
        continue;
      if (strcasecmp(h, host) || p != port)
        continue;
      found = 1;
      if (!strcmp(alg, "ssh-ed25519") && !strcmp(key, want))
        same = 1;
    }
    fclose(f);
  }
  if (same)
    return 1;
  if (found)
  {
    say("\n@@@ WARNING: THE HOST KEY OF %s HAS CHANGED! @@@\n"
        "Someone could be intercepting the connection, or the host was\n"
        "reinstalled. New key: %s\n"
        "If that is expected, remove the line for %s from %s.\n", host, fp, host, path);
    return 0;
  }
  say("The host key of %s (port %d) is not known yet:\n  ED25519 %s\n", host, port, fp);
  if (!accept_new)
  {
    read_line("Connect and remember it (yes/no)? ", answer, sizeof(answer), 1);
    if (strcasecmp(answer, "yes") && strcasecmp(answer, "y"))
      return 0;
  }
  f = fopen(path, "a");
  if (f)
  {
    fprintf(f, "%s %d ssh-ed25519 %s\n", host, port, want);
    fclose(f);
    say("Added to %s.\n", path);
  }
  return 1;
}

/* ---------------------------------------------------------------- */
/* authentication                                                   */

static void send_auth_key(void)
{
  struct buf b, data, sig;
  uint8_t sg[64];

  buf_init(&b);
  buf_init(&data);
  buf_init(&sig);
  put_u8(&b, SSH_MSG_USERAUTH_REQUEST);
  put_cstr(&b, user);
  put_cstr(&b, "ssh-connection");
  put_cstr(&b, "publickey");
  put_u8(&b, 1);
  put_cstr(&b, "ssh-ed25519");
  {
    struct buf blob;
    buf_init(&blob);
    ssh_ed25519_blob(&blob, key_pk);
    put_str(&b, blob.d, blob.len);
    put_str(&data, s.sid, 32);
    put_bytes(&data, b.d, b.len);       /* the request so far */
    buf_free(&blob);
  }
  crypto_ed25519_sign(sg, key_sk, data.d, data.len);
  put_cstr(&sig, "ssh-ed25519");
  put_str(&sig, sg, 64);
  put_str(&b, sig.d, sig.len);
  ssh_send(&s, &b);
  buf_free(&b);
  buf_free(&data);
  buf_free(&sig);
}

static void send_auth(const char *method)
{
  struct buf b;

  buf_init(&b);
  put_u8(&b, SSH_MSG_USERAUTH_REQUEST);
  put_cstr(&b, user);
  put_cstr(&b, "ssh-connection");
  put_cstr(&b, method);
  if (!strcmp(method, "password"))
  {
    char pw[128];
    char prompt[300];
    snprintf(prompt, sizeof(prompt), "%s@%s's password: ", user, host);
    read_line(prompt, pw, sizeof(pw), 0);
    put_u8(&b, 0);
    put_cstr(&b, pw);
    crypto_wipe(pw, sizeof(pw));
  }
  else if (!strcmp(method, "keyboard-interactive"))
  {
    put_cstr(&b, "");                   /* language */
    put_cstr(&b, "");                   /* submethods */
  }
  ssh_send(&s, &b);
  buf_free(&b);
}

/* the server's list of methods that can continue: pick the next one */
static void auth_next(struct buf *p)
{
  uint32_t n;
  const uint8_t *list = get_str(p, &n);

  if (auth_state == A_KEY)
    say("The server did not accept the key %s.\n", keypath);
  if (list && namelist_has(list, n, "password") && pw_tries < 3)
  {
    pw_tries++;
    auth_state = A_PASSWORD;
    send_auth("password");
  }
  else if (list && namelist_has(list, n, "keyboard-interactive") && pw_tries < 3)
  {
    pw_tries++;
    auth_state = A_KBDINT;
    send_auth("keyboard-interactive");
  }
  else
  {
    say("Permission denied (%.*s).\n", (int)n, list ? (const char *)list : "");
    ssh_disconnect(&s, SSH_DISCONNECT_NO_MORE_AUTH_METHODS, "no more methods");
  }
}

/* keyboard-interactive: the server's questions */
static void kbd_info(struct buf *p)
{
  char name[128], instr[256], prompt[256], ans[128];
  uint32_t i, num;
  struct buf b;

  get_cstr(p, name, sizeof(name));
  get_cstr(p, instr, sizeof(instr));
  get_str(p, &i);                       /* language */
  num = get_u32(p);
  if (name[0])
    say("%s\n", name);
  if (instr[0])
    say("%s\n", instr);
  buf_init(&b);
  put_u8(&b, 61);                       /* SSH_MSG_USERAUTH_INFO_RESPONSE */
  put_u32(&b, num);
  for (i = 0; i < num && i < 8; i++)
  {
    int echo;
    get_cstr(p, prompt, sizeof(prompt));
    echo = get_u8(p);
    read_line(prompt, ans, sizeof(ans), echo);
    put_cstr(&b, ans);
    crypto_wipe(ans, sizeof(ans));
  }
  ssh_send(&s, &b);
  buf_free(&b);
}

/* ---------------------------------------------------------------- */
/* the session channel                                              */

static void open_session(void)
{
  struct buf b;

  buf_init(&b);
  put_u8(&b, SSH_MSG_CHANNEL_OPEN);
  put_cstr(&b, "session");
  put_u32(&b, 0);
  put_u32(&b, WINDOW);
  put_u32(&b, MAX_PKT);
  ssh_send(&s, &b);
  buf_free(&b);
}

static void start_session(void)
{
  struct buf b;

  buf_init(&b);
  if (want_tty)
  {
    put_u8(&b, SSH_MSG_CHANNEL_REQUEST);
    put_u32(&b, peer_chan);
    put_cstr(&b, "pty-req");
    put_u8(&b, 0);
    put_cstr(&b, "xterm");
    put_u32(&b, TERM_COLS);
    put_u32(&b, TERM_ROWS);
    put_u32(&b, 0);
    put_u32(&b, 0);
    put_cstr(&b, "");                   /* terminal modes: none */
    ssh_send(&s, &b);
  }
  put_u8(&b, SSH_MSG_CHANNEL_REQUEST);
  put_u32(&b, peer_chan);
  put_cstr(&b, cmd[0] ? "exec" : "shell");
  put_u8(&b, 1);
  if (cmd[0])
    put_cstr(&b, cmd);
  ssh_send(&s, &b);
  buf_free(&b);
}

static void send_data(const uint8_t *d, size_t n)
{
  struct buf b;

  buf_init(&b);
  while (n && chan_open && !chan_closed)
  {
    size_t k = n > 16384 ? 16384 : n;
    if (k > peer_window)
      break;                            /* rare with interactive input */
    put_u8(&b, SSH_MSG_CHANNEL_DATA);
    put_u32(&b, peer_chan);
    put_str(&b, d, k);
    ssh_send(&s, &b);
    peer_window -= (uint32_t)k;
    d += k;
    n -= k;
  }
  buf_free(&b);
}

static uint32_t window_used;

static void got_data(const uint8_t *d, uint32_t n, int err)
{
  if (want_tty)
    term_write(&term, d, n);
  else
  {
    fwrite(d, 1, n, err ? stderr : stdout);
    fflush(err ? stderr : stdout);
  }
  window_used += n;
  if (window_used > WINDOW / 2)
  {
    struct buf b;
    buf_init(&b);
    put_u8(&b, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    put_u32(&b, peer_chan);
    put_u32(&b, window_used);
    ssh_send(&s, &b);
    buf_free(&b);
    window_used = 0;
  }
}

static void handle(struct buf *p)
{
  uint8_t t = p->d[0];
  struct buf b;
  uint32_t n;
  const uint8_t *d;

  buf_init(&b);
  switch (t)
  {
    case SSH_MSG_SERVICE_ACCEPT:
      if (have_key)
      {
        auth_state = A_KEY;
        send_auth_key();
      }
      else
      {
        auth_state = A_NONE;
        send_auth("none");
      }
      break;
    case SSH_MSG_USERAUTH_FAILURE:
      auth_next(p);
      break;
    case SSH_MSG_USERAUTH_BANNER:
    {
      char msg[1024];
      if (get_cstr(p, msg, sizeof(msg)))
        say("%s", msg);
      break;
    }
    case 60:                            /* PK_OK / INFO_REQUEST */
      if (auth_state == A_KBDINT)
        kbd_info(p);
      break;
    case SSH_MSG_USERAUTH_SUCCESS:
      auth_state = A_DONE;
      open_session();
      break;
    case SSH_MSG_GLOBAL_REQUEST:
    {
      char name[64];
      get_cstr(p, name, sizeof(name));
      if (get_u8(p))
      {
        put_u8(&b, SSH_MSG_REQUEST_FAILURE);
        ssh_send(&s, &b);
      }
      break;
    }
    case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
      get_u32(p);
      peer_chan = get_u32(p);
      peer_window = get_u32(p);
      peer_max = get_u32(p);
      chan_open = 1;
      start_session();
      break;
    case SSH_MSG_CHANNEL_OPEN_FAILURE:
    {
      char msg[128];
      get_u32(p);
      get_u32(p);
      get_cstr(p, msg, sizeof(msg));
      say("The server refused the session: %s\n", msg);
      ssh_disconnect(&s, SSH_DISCONNECT_BY_APPLICATION, "no session");
      break;
    }
    case SSH_MSG_CHANNEL_WINDOW_ADJUST:
      get_u32(p);
      peer_window += get_u32(p);
      break;
    case SSH_MSG_CHANNEL_DATA:
      get_u32(p);
      d = get_str(p, &n);
      if (d)
        got_data(d, n, 0);
      break;
    case SSH_MSG_CHANNEL_EXTENDED_DATA:
      get_u32(p);
      get_u32(p);
      d = get_str(p, &n);
      if (d)
        got_data(d, n, 1);
      break;
    case SSH_MSG_CHANNEL_REQUEST:
    {
      char req[32];
      int want;
      get_u32(p);
      get_cstr(p, req, sizeof(req));
      want = get_u8(p);
      if (!strcmp(req, "exit-status"))
      {
        exit_code = (int)get_u32(p);
        got_exit = 1;
      }
      if (want)
      {
        put_u8(&b, SSH_MSG_CHANNEL_FAILURE);
        put_u32(&b, peer_chan);
        ssh_send(&s, &b);
      }
      break;
    }
    case SSH_MSG_CHANNEL_FAILURE:
      say("The server refused the %s request.\n", cmd[0] ? "exec" : "shell");
      break;
    case SSH_MSG_CHANNEL_SUCCESS:
    case SSH_MSG_CHANNEL_EOF:
    case SSH_MSG_REQUEST_FAILURE:
    case SSH_MSG_REQUEST_SUCCESS:
      break;
    case SSH_MSG_CHANNEL_CLOSE:
      if (!chan_closed)
      {
        put_u8(&b, SSH_MSG_CHANNEL_CLOSE);
        put_u32(&b, peer_chan);
        ssh_send(&s, &b);
      }
      chan_closed = 1;
      break;
    default:
      put_u8(&b, SSH_MSG_UNIMPLEMENTED);
      put_u32(&b, s.seq_in - 1);
      ssh_send(&s, &b);
  }
  buf_free(&b);
}

/* ---------------------------------------------------------------- */

static int keygen(void)
{
  uint8_t seed[32], sk[64], pk[32];
  char line[200];

  rng_bytes(seed, 32);
  crypto_ed25519_key_pair(sk, pk, seed);
  if (!key_write(keypath, sk, pk, "dos@modernDOS"))
  {
    say("Cannot write %s\n", keypath);
    return 1;
  }
  crypto_wipe(sk, sizeof(sk));
  key_public_line(pk, "dos@modernDOS", line, sizeof(line));
  say("Made %s. Add this line to ~/.ssh/authorized_keys on the server:\n", keypath);
  printf("%s\n", line);
  return 0;
}

static void usage(void)
{
  say("Usage: SSH [/P port] [/I keyfile] [/L user] [/Y] [/T] [user@]host [command]\n"
      "       SSH /KEYGEN\n");
  exit(2);
}

int main(int argc, char **argv)
{
  char seed[128], *slash, err[160];
  int i, keygen_only = 0, escape_state = 1;
  uint32_t last_draw = 0;
  struct buf p, keys;

  snprintf(dir, sizeof(dir), "%s", argv[0]);
  for (i = 0; dir[i]; i++)
    if (dir[i] == '\\')
      dir[i] = '/';
  slash = strrchr(dir, '/');
  if (slash)
    slash[1] = 0;
  else
    dir[0] = 0;
  plat_prog = "ssh";
  path_of(seed, sizeof(seed), "SEED.BIN");
  path_of(keypath, sizeof(keypath), "ID.KEY");

  for (i = 1; i < argc; i++)
  {
    const char *a = argv[i];
    if (!host[0] && (a[0] == '/' || a[0] == '-') && a[1])
    {
      char o = (char)toupper((unsigned char)a[1]);
      if (!strcasecmp(a + 1, "KEYGEN"))
        keygen_only = 1;
      else if (o == 'P' && i + 1 < argc)
        port = atoi(argv[++i]);
      else if (o == 'I' && i + 1 < argc)
        snprintf(keypath, sizeof(keypath), "%s", argv[++i]);
      else if (o == 'L' && i + 1 < argc)
        snprintf(user, sizeof(user), "%s", argv[++i]);
      else if (o == 'Y')
        accept_new = 1;
      else if (o == 'T')
        want_tty = 1;
      else
        usage();
    }
    else if (!host[0])
    {
      const char *at = strchr(a, '@');
      if (at)
      {
        snprintf(user, sizeof(user), "%.*s", (int)(at - a), a);
        snprintf(host, sizeof(host), "%s", at + 1);
      }
      else
        snprintf(host, sizeof(host), "%s", a);
    }
    else
    {
      size_t n = strlen(cmd);
      snprintf(cmd + n, sizeof(cmd) - n, "%s%s", n ? " " : "", a);
    }
  }
  rng_init(seed);
  if (keygen_only)
    return keygen();
  if (!host[0])
    usage();
  if (!user[0])
    strcpy(user, "dos");
  if (!cmd[0])
    want_tty = 1;
  have_key = key_read(keypath, key_sk, key_pk, err, sizeof(err));
  if (!have_key && strstr(err, "cannot open") == NULL)
    say("%s\n", err);

  if (!plat_init())
    return 255;
  say("Connecting to %s port %d...\n", host, port);
  {
    int fd = plat_connect(host, port);
    if (fd < 0)
    {
      say("Cannot connect to %s port %d.\n", host, port);
      return 255;
    }
    s.check_hostkey = check_hostkey;
    ssh_start(&s, fd, 0);
  }
  buf_init(&p);
  buf_init(&keys);
  buf_init(&replies);
  term_init(&term, &replies);

  while (!s.dead && !chan_closed)
  {
    int busy = ssh_io(&s), r, k;
    while ((r = ssh_packet(&s, &p)) > 0)
    {
      handle(&p);
      busy = 1;
    }
    if (ssh_ready(&s) && !service_sent)
    {
      struct buf b;
      buf_init(&b);
      put_u8(&b, SSH_MSG_SERVICE_REQUEST);
      put_cstr(&b, "ssh-userauth");
      ssh_send(&s, &b);
      buf_free(&b);
      service_sent = 1;                 /* SERVICE_ACCEPT starts the login */
    }
    if (chan_open && want_tty)
    {
      /* keys: ~. at the start of a line disconnects */
      while ((k = plat_bioskey()) >= 0)
      {
        uint8_t ascii = (uint8_t)k;
        if (escape_state == 2 && ascii == '.')
        {
          say("\r\nDisconnected.\r\n");
          ssh_disconnect(&s, SSH_DISCONNECT_BY_APPLICATION, "disconnected by user");
          break;
        }
        if (escape_state == 1 && ascii == '~')
        {
          escape_state = 2;
          continue;
        }
        if (escape_state == 2)
          term_key(&term, '~', &keys);
        escape_state = ascii == '\r';
        term_key(&term, (uint16_t)k, &keys);
      }
      if (replies.len)
      {
        put_bytes(&keys, replies.d, replies.len);
        buf_reset(&replies);
      }
      if (keys.len)
      {
        send_data(keys.d, keys.len);
        buf_reset(&keys);
        busy = 1;
      }
      if (term.dirty && plat_ms() - last_draw >= 30)
      {
        plat_screen_show(&term.cell[0][0], term.row, term.col, term.cursor_on);
        term.dirty = 0;
        last_draw = plat_ms();
      }
    }
    busy |= ssh_io(&s);
    if (!busy)
      plat_idle();
  }
  if (want_tty && chan_open)
  {
    plat_screen_show(&term.cell[0][0], term.row, term.col, 1);
    if (getenv("SSHDUMP"))              /* tests: the final screen to the log */
      for (i = 0; i < TERM_ROWS; i++)
      {
        char line[TERM_COLS + 1];
        int c;
        for (c = 0; c < TERM_COLS; c++)
          line[c] = (char)(term.cell[i][c] & 0xff);
        while (c > 0 && line[c - 1] == ' ')
          c--;
        line[c] = 0;
        if (c)
          plat_log("screen: %s\n", line);
      }
    printf("\n");
  }
  ssh_io(&s);
  if (s.dead && !chan_closed && s.why[0])
    say("Connection to %s closed: %s\n", host, s.why);
  plat_close(s.fd);
  ssh_free(&s);
  rng_save(seed);
  crypto_wipe(key_sk, sizeof(key_sk));
  return got_exit ? exit_code : (chan_closed ? 0 : 255);
}
