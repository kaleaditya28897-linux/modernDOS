/* SSH transport layer: packets, encryption, key exchange (see ssh.h) */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "crypto/monocypher.h"
#include "crypto/monocypher-ed25519.h"
#include "sha256.h"
#include "rng.h"
#include "ssh.h"

#define VERSION "SSH-2.0-modernDOS_0.1"

#define KEX_ALGS_S "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com"
#define KEX_ALGS_C "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com"
#define HOSTKEY_ALG "ssh-ed25519"
#define CIPHER "chacha20-poly1305@openssh.com"
#define MAC "hmac-sha2-256"                     /* unused with an AEAD cipher */
#define COMP "none"

static void fail(struct ssh *s, uint32_t code, const char *fmt, ...)
{
  va_list ap;

  if (s->dead)
    return;
  va_start(ap, fmt);
  vsnprintf(s->why, sizeof(s->why), fmt, ap);
  va_end(ap);
  ssh_disconnect(s, code, s->why);
}

void ssh_start(struct ssh *s, int fd, int server)
{
  const uint8_t *hsk = s->host_sk, *hpk = s->host_pk;
  int (*check)(struct ssh *, const uint8_t *) = s->check_hostkey;
  void *user = s->user;

  memset(s, 0, sizeof(*s));
  s->host_sk = hsk;
  s->host_pk = hpk;
  s->check_hostkey = check;
  s->user = user;
  s->fd = fd;
  s->server = server;
  buf_init(&s->in);
  buf_init(&s->out);
  buf_init(&s->deferred);
  buf_init(&s->i_c);
  buf_init(&s->i_s);
  strcpy(server ? s->vs : s->vc, VERSION);
  put_bytes(&s->out, VERSION "\r\n", strlen(VERSION) + 2);
}

void ssh_free(struct ssh *s)
{
  buf_free(&s->in);
  buf_free(&s->out);
  buf_free(&s->deferred);
  buf_free(&s->i_c);
  buf_free(&s->i_s);
  crypto_wipe(&s->rx, sizeof(s->rx));
  crypto_wipe(&s->tx, sizeof(s->tx));
  crypto_wipe(&s->nrx, sizeof(s->nrx));
  crypto_wipe(&s->ntx, sizeof(s->ntx));
  crypto_wipe(s->x_sk, sizeof(s->x_sk));
}

int ssh_ready(const struct ssh *s)
{
  return !s->dead && s->kex_done && !s->sent_kexinit && !s->got_kexinit;
}

size_t ssh_pending(const struct ssh *s)
{
  return s->out.len + s->deferred.len;
}

int ssh_io(struct ssh *s)
{
  uint8_t tmp[4096];
  int n, moved = 0;

  if (s->dead && !s->out.len)
    return 0;
  while (s->out.len)
  {
    n = net_send(s->fd, s->out.d, (int)s->out.len);
    if (n < 0)
    {
      s->dead = 1;
      if (!s->why[0])
        strcpy(s->why, "connection lost");
      buf_reset(&s->out);
      return 1;
    }
    if (n == 0)
      break;
    buf_consume(&s->out, (size_t)n);
    moved = 1;
  }
  while (!s->dead && s->in.len < 2 * SSH_MAX_PACKET)
  {
    n = net_recv(s->fd, tmp, sizeof(tmp));
    if (n < 0)
    {
      s->dead = 1;
      if (!s->why[0])
        strcpy(s->why, "connection closed by peer");
      return 1;
    }
    if (n == 0)
      break;
    put_bytes(&s->in, tmp, (size_t)n);
    rng_event((uint32_t)n);
    moved = 1;
  }
  return moved;
}

/* ---------------------------------------------------------------- */
/* packets                                                          */

static void nonce_of(uint32_t seq, uint8_t n[8])
{
  memset(n, 0, 4);
  poke_u32(n + 4, seq);
}

static void send_now(struct ssh *s, const uint8_t *payload, size_t plen)
{
  size_t start = s->out.len, len;
  uint8_t pad[32], *pkt;
  int padlen;

  if (s->tx_on)
    padlen = 8 - (int)((1 + plen) % 8);
  else
    padlen = 8 - (int)((5 + plen) % 8);
  if (padlen < 4)
    padlen += 8;
  rng_bytes(pad, (size_t)padlen);
  len = 1 + plen + (size_t)padlen;
  put_u32(&s->out, (uint32_t)len);
  put_u8(&s->out, (uint8_t)padlen);
  put_bytes(&s->out, payload, plen);
  put_bytes(&s->out, pad, (size_t)padlen);
  if (s->tx_on)
  {
    uint8_t nonce[8], polykey[32], tag[16];
    buf_reserve(&s->out, 16);
    pkt = s->out.d + start;
    nonce_of(s->seq_out, nonce);
    crypto_chacha20_djb(pkt, pkt, 4, s->tx.hdr, nonce, 0);
    crypto_chacha20_djb(pkt + 4, pkt + 4, len, s->tx.main, nonce, 1);
    memset(polykey, 0, sizeof(polykey));
    crypto_chacha20_djb(polykey, polykey, sizeof(polykey), s->tx.main, nonce, 0);
    crypto_poly1305(tag, pkt, 4 + len, polykey);
    put_bytes(&s->out, tag, 16);
    crypto_wipe(polykey, sizeof(polykey));
  }
  s->seq_out++;
}

static int kex_msg(uint8_t t)
{
  return (t >= 1 && t <= 4) || (t >= 20 && t <= 49);
}

void ssh_send(struct ssh *s, struct buf *payload)
{
  if (s->dead || !payload->len)
  {
    buf_reset(payload);
    return;
  }
  /* during a key exchange only transport messages go out */
  if ((s->sent_kexinit || s->got_kexinit || !s->kex_done) && !kex_msg(payload->d[0]))
  {
    put_u32(&s->deferred, (uint32_t)payload->len);
    put_bytes(&s->deferred, payload->d, payload->len);
  }
  else
    send_now(s, payload->d, payload->len);
  buf_reset(payload);
}

void ssh_disconnect(struct ssh *s, uint32_t code, const char *msg)
{
  struct buf b;

  if (s->dead)
    return;
  buf_init(&b);
  put_u8(&b, SSH_MSG_DISCONNECT);
  put_u32(&b, code);
  put_cstr(&b, msg);
  put_cstr(&b, "");
  if (s->version_done)
    send_now(s, b.d, b.len);
  buf_free(&b);
  if (!s->why[0])
    snprintf(s->why, sizeof(s->why), "%s", msg);
  s->dead = 1;
}

/* one packet from s->in into `payload` (type byte first): 1 yes, 0 not
   complete yet, -1 broken */
static int read_packet(struct ssh *s, struct buf *payload)
{
  uint8_t lenbytes[4], nonce[8];
  uint32_t len, total, padlen;

  if (s->in.len < 4)
    return 0;
  nonce_of(s->seq_in, nonce);
  memcpy(lenbytes, s->in.d, 4);
  if (s->rx_on)
    crypto_chacha20_djb(lenbytes, lenbytes, 4, s->rx.hdr, nonce, 0);
  len = peek_u32(lenbytes);
  if (len < 8 || len > SSH_MAX_PACKET || (s->rx_on ? len : len + 4) % 8)
  {
    fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "bad packet length %lu", (unsigned long)len);
    return -1;
  }
  total = 4 + len + (s->rx_on ? 16 : 0);
  if (s->in.len < total)
    return 0;
  if (s->rx_on)
  {
    uint8_t polykey[32], tag[16];
    memset(polykey, 0, sizeof(polykey));
    crypto_chacha20_djb(polykey, polykey, sizeof(polykey), s->rx.main, nonce, 0);
    crypto_poly1305(tag, s->in.d, 4 + len, polykey);
    crypto_wipe(polykey, sizeof(polykey));
    if (crypto_verify16(tag, s->in.d + 4 + len))
    {
      fail(s, SSH_DISCONNECT_MAC_ERROR, "packet authentication failed");
      return -1;
    }
    crypto_chacha20_djb(s->in.d + 4, s->in.d + 4, len, s->rx.main, nonce, 1);
  }
  padlen = s->in.d[4];
  if (padlen < 4 || padlen + 1 >= len)
  {
    fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "bad padding");
    return -1;
  }
  buf_reset(payload);
  put_bytes(payload, s->in.d + 5, len - padlen - 1);
  buf_consume(&s->in, total);
  s->seq_in++;
  return 1;
}

/* ---------------------------------------------------------------- */
/* key exchange                                                     */

static void send_kexinit(struct ssh *s)
{
  struct buf b, *mine = s->server ? &s->i_s : &s->i_c;
  uint8_t cookie[16];

  buf_init(&b);
  put_u8(&b, SSH_MSG_KEXINIT);
  rng_bytes(cookie, sizeof(cookie));
  put_bytes(&b, cookie, sizeof(cookie));
  /* the strict-KEX marker only in the first KEXINIT */
  put_cstr(&b, s->kex_done ? "curve25519-sha256,curve25519-sha256@libssh.org"
                           : s->server ? KEX_ALGS_S : KEX_ALGS_C);
  put_cstr(&b, HOSTKEY_ALG);
  put_cstr(&b, CIPHER);
  put_cstr(&b, CIPHER);
  put_cstr(&b, MAC);
  put_cstr(&b, MAC);
  put_cstr(&b, COMP);
  put_cstr(&b, COMP);
  put_cstr(&b, "");
  put_cstr(&b, "");
  put_u8(&b, 0);                        /* first_kex_packet_follows */
  put_u32(&b, 0);
  buf_reset(mine);
  put_bytes(mine, b.d, b.len);
  send_now(s, b.d, b.len);
  buf_free(&b);
  s->sent_kexinit = 1;
}

/* the peer's KEXINIT: check that our algorithms are acceptable */
static int got_kexinit(struct ssh *s, struct buf *p)
{
  const uint8_t *l[10];
  uint32_t n[10];
  int i, follows;
  struct buf *theirs = s->server ? &s->i_c : &s->i_s;

  buf_reset(theirs);
  put_bytes(theirs, p->d, p->len);
  p->pos = 1;
  get_bytes(p, 16);
  for (i = 0; i < 10; i++)
    l[i] = get_str(p, &n[i]);
  follows = get_u8(p);
  if (p->err)
  {
    fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "bad KEXINIT");
    return 0;
  }
  if (!namelist_has(l[0], n[0], "curve25519-sha256") &&
      !namelist_has(l[0], n[0], "curve25519-sha256@libssh.org"))
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "no common key exchange (need curve25519-sha256)");
    return 0;
  }
  if (!namelist_has(l[1], n[1], HOSTKEY_ALG))
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "no common host key type (need ssh-ed25519)");
    return 0;
  }
  if (!namelist_has(l[2], n[2], CIPHER) || !namelist_has(l[3], n[3], CIPHER))
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "no common cipher (need " CIPHER ")");
    return 0;
  }
  if (!namelist_has(l[6], n[6], COMP) || !namelist_has(l[7], n[7], COMP))
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "no common compression");
    return 0;
  }
  if (!s->kex_done)
    s->strict = namelist_has(l[0], n[0], s->server ? "kex-strict-c-v00@openssh.com"
                                                   : "kex-strict-s-v00@openssh.com");
  /* a guessed first kex packet is only right if both lists start alike */
  if (follows)
  {
    const char *first = "curve25519-sha256,";
    s->ignore_next = n[0] < strlen(first) || memcmp(l[0], first, strlen(first)) ||
                     n[1] < strlen(HOSTKEY_ALG) || memcmp(l[1], HOSTKEY_ALG, strlen(HOSTKEY_ALG));
  }
  s->got_kexinit = 1;
  return 1;
}

/* exchange hash and the four keys */
static void kex_finish(struct ssh *s, const uint8_t *ks, size_t ks_len, const uint8_t qc[32],
                       const uint8_t qs[32], const uint8_t k[32], uint8_t h[32])
{
  struct buf b, kmp;
  struct sha256 c;
  uint8_t k1[32], k2[32];
  int i;
  const char letters[4] = { 'C', 'D', 0, 0 };

  buf_init(&b);
  buf_init(&kmp);
  put_cstr(&b, s->vc);
  put_cstr(&b, s->vs);
  put_str(&b, s->i_c.d, s->i_c.len);
  put_str(&b, s->i_s.d, s->i_s.len);
  put_str(&b, ks, ks_len);
  put_str(&b, qc, 32);
  put_str(&b, qs, 32);
  put_mpint(&kmp, k, 32);
  put_bytes(&b, kmp.d, kmp.len);
  sha256(b.d, b.len, h);
  if (!s->have_sid)
  {
    memcpy(s->sid, h, 32);
    s->have_sid = 1;
  }
  /* C: client to server key, D: server to client (64 bytes each) */
  for (i = 0; i < 2; i++)
  {
    struct ssh_keys *dst = (i == 0) == s->server ? &s->nrx : &s->ntx;
    sha256_init(&c);
    sha256_update(&c, kmp.d, kmp.len);
    sha256_update(&c, h, 32);
    sha256_update(&c, &letters[i], 1);
    sha256_update(&c, s->sid, 32);
    sha256_final(&c, k1);
    sha256_init(&c);
    sha256_update(&c, kmp.d, kmp.len);
    sha256_update(&c, h, 32);
    sha256_update(&c, k1, 32);
    sha256_final(&c, k2);
    memcpy(dst->main, k1, 32);          /* K_2: first half of the 64 bytes */
    memcpy(dst->hdr, k2, 32);           /* K_1: second half */
  }
  crypto_wipe(k1, sizeof(k1));
  crypto_wipe(k2, sizeof(k2));
  buf_free(&kmp);
  buf_free(&b);
}

static void send_newkeys(struct ssh *s)
{
  uint8_t m = SSH_MSG_NEWKEYS;

  send_now(s, &m, 1);
  s->tx = s->ntx;
  s->tx_on = 1;
  crypto_wipe(&s->ntx, sizeof(s->ntx));
  if (s->strict)
    s->seq_out = 0;
  s->sent_newkeys = 1;
}

static int all_zero(const uint8_t *p, int n)
{
  uint8_t acc = 0;
  while (n--)
    acc |= *p++;
  return !acc;
}

/* server: the client's ephemeral key */
static void ecdh_init(struct ssh *s, struct buf *p)
{
  uint32_t n;
  const uint8_t *qc = get_str(p, &n);
  uint8_t k[32], h[32], sig[64];
  struct buf ks, r, sigb;

  if (!qc || n != 32)
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad ECDH_INIT");
    return;
  }
  rng_bytes(s->x_sk, 32);
  crypto_x25519_public_key(s->x_pk, s->x_sk);
  crypto_x25519(k, s->x_sk, qc);
  crypto_wipe(s->x_sk, 32);
  if (all_zero(k, 32))
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad client key");
    return;
  }
  buf_init(&ks);
  buf_init(&r);
  buf_init(&sigb);
  ssh_ed25519_blob(&ks, s->host_pk);
  kex_finish(s, ks.d, ks.len, qc, s->x_pk, k, h);
  crypto_ed25519_sign(sig, s->host_sk, h, 32);
  put_cstr(&sigb, HOSTKEY_ALG);
  put_str(&sigb, sig, 64);
  put_u8(&r, SSH_MSG_KEX_ECDH_REPLY);
  put_str(&r, ks.d, ks.len);
  put_str(&r, s->x_pk, 32);
  put_str(&r, sigb.d, sigb.len);
  send_now(s, r.d, r.len);
  send_newkeys(s);
  crypto_wipe(k, sizeof(k));
  buf_free(&ks);
  buf_free(&r);
  buf_free(&sigb);
}

static void send_ecdh_init(struct ssh *s)
{
  struct buf b;

  rng_bytes(s->x_sk, 32);
  crypto_x25519_public_key(s->x_pk, s->x_sk);
  buf_init(&b);
  put_u8(&b, SSH_MSG_KEX_ECDH_INIT);
  put_str(&b, s->x_pk, 32);
  send_now(s, b.d, b.len);
  buf_free(&b);
}

/* client: the server's reply */
static void ecdh_reply(struct ssh *s, struct buf *p)
{
  uint32_t ksn, qsn, sn, an, pkn, sgn;
  const uint8_t *ks = get_str(p, &ksn), *qs = get_str(p, &qsn), *sig = get_str(p, &sn);
  const uint8_t *alg, *pk, *sg;
  struct buf kb, sb;
  uint8_t k[32], h[32];

  if (p->err || qsn != 32)
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad ECDH_REPLY");
    return;
  }
  kb.d = (uint8_t *)ks; kb.len = ksn; kb.pos = 0; kb.err = 0; kb.cap = 0;
  alg = get_str(&kb, &an);
  pk = get_str(&kb, &pkn);
  sb.d = (uint8_t *)sig; sb.len = sn; sb.pos = 0; sb.err = 0; sb.cap = 0;
  get_str(&sb, &an);
  sg = get_str(&sb, &sgn);
  if (kb.err || sb.err || an != strlen(HOSTKEY_ALG) || memcmp(alg, HOSTKEY_ALG, an) ||
      pkn != 32 || sgn != 64)
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "unsupported host key");
    return;
  }
  crypto_x25519(k, s->x_sk, qs);
  crypto_wipe(s->x_sk, 32);
  if (all_zero(k, 32))
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad server key");
    return;
  }
  kex_finish(s, ks, ksn, s->x_pk, qs, k, h);
  crypto_wipe(k, sizeof(k));
  if (crypto_ed25519_check(sg, pk, h, 32))
  {
    fail(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "host key signature is wrong");
    return;
  }
  if (s->kex_done && memcmp(pk, s->peer_hostkey, 32))
  {
    fail(s, SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE, "host key changed during re-key");
    return;
  }
  memcpy(s->peer_hostkey, pk, 32);
  if (!s->kex_done && s->check_hostkey && !s->check_hostkey(s, pk))
  {
    fail(s, SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE, "host key not accepted");
    return;
  }
  send_newkeys(s);
}

static void got_newkeys(struct ssh *s)
{
  size_t i = 0;

  s->rx = s->nrx;
  s->rx_on = 1;
  crypto_wipe(&s->nrx, sizeof(s->nrx));
  if (s->strict)
    s->seq_in = 0;
  s->kex_done = 1;
  s->sent_kexinit = s->got_kexinit = s->sent_newkeys = 0;
  /* what the caller sent meanwhile */
  while (i + 4 <= s->deferred.len)
  {
    uint32_t n = peek_u32(s->deferred.d + i);
    send_now(s, s->deferred.d + i + 4, n);
    i += 4 + n;
  }
  buf_reset(&s->deferred);
}

/* identification line of the peer */
static int read_version(struct ssh *s)
{
  for (;;)
  {
    uint8_t *nl = memchr(s->in.d, '\n', s->in.len);
    size_t n;
    char *dst = s->server ? s->vc : s->vs;
    if (!nl)
    {
      if (s->in.len > 255)
      {
        fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "no SSH identification");
        return -1;
      }
      return 0;
    }
    n = (size_t)(nl - s->in.d);
    if (n && nl[-1] == '\r')
      n--;
    if (n >= 8 && !memcmp(s->in.d, "SSH-", 4))
    {
      if (memcmp(s->in.d, "SSH-2.0-", 8) && memcmp(s->in.d, "SSH-1.99-", 9))
      {
        fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "SSH protocol 2 only");
        return -1;
      }
      if (n > 250)
        n = 250;
      memcpy(dst, s->in.d, n);
      dst[n] = 0;
      buf_consume(&s->in, (size_t)(nl - s->in.d) + 1);
      s->version_done = 1;
      return 1;
    }
    if (s->server)                      /* clients send nothing before it */
    {
      fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "no SSH identification");
      return -1;
    }
    buf_consume(&s->in, (size_t)(nl - s->in.d) + 1);   /* server banner line */
  }
}

int ssh_packet(struct ssh *s, struct buf *payload)
{
  for (;;)
  {
    int r;
    uint8_t t;

    if (s->dead)
      return -1;
    if (!s->version_done)
    {
      r = read_version(s);
      if (r <= 0)
        return r;
      if (!s->sent_kexinit)
        send_kexinit(s);
    }
    r = read_packet(s, payload);
    if (r <= 0)
      return r;
    t = payload->d[0];
    payload->pos = 1;
    if (s->ignore_next && t >= 30 && t <= 49)
    {
      s->ignore_next = 0;
      continue;
    }
    /* strict KEX: nothing else during the first key exchange */
    if (s->strict && !s->kex_done && t != SSH_MSG_KEXINIT && t != SSH_MSG_NEWKEYS &&
        t != SSH_MSG_KEX_ECDH_INIT && t != SSH_MSG_KEX_ECDH_REPLY && t != SSH_MSG_DISCONNECT)
    {
      fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected message %u during key exchange", t);
      return -1;
    }
    switch (t)
    {
      case SSH_MSG_DISCONNECT:
      {
        char msg[120];
        get_u32(payload);
        get_cstr(payload, msg, sizeof(msg));
        snprintf(s->why, sizeof(s->why), "peer disconnected: %s", msg);
        s->dead = 1;
        return -1;
      }
      case SSH_MSG_IGNORE:
      case SSH_MSG_DEBUG:
      case SSH_MSG_UNIMPLEMENTED:
        continue;
      case SSH_MSG_KEXINIT:
        if (s->got_kexinit)
        {
          fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "second KEXINIT");
          return -1;
        }
        if (!got_kexinit(s, payload))
          return -1;
        if (!s->sent_kexinit)
          send_kexinit(s);
        if (!s->server)
          send_ecdh_init(s);
        continue;
      case SSH_MSG_KEX_ECDH_INIT:
        if (!s->server || !s->got_kexinit || s->sent_newkeys)
          break;
        ecdh_init(s, payload);
        continue;
      case SSH_MSG_KEX_ECDH_REPLY:
        if (s->server || !s->got_kexinit || s->sent_newkeys)
          break;
        ecdh_reply(s, payload);
        continue;
      case SSH_MSG_NEWKEYS:
        if (!s->sent_newkeys)
          break;
        got_newkeys(s);
        continue;
      default:
        if (!s->kex_done || (t >= 20 && t <= 49))
          break;                        /* not allowed before / during kex */
        return 1;
    }
    fail(s, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected message %u", t);
    return -1;
  }
}

/* ---------------------------------------------------------------- */

void ssh_ed25519_blob(struct buf *b, const uint8_t pk[32])
{
  put_cstr(b, HOSTKEY_ALG);
  put_str(b, pk, 32);
}

void ssh_fingerprint(const uint8_t pk[32], char out[64])
{
  struct buf b;
  uint8_t h[32];

  buf_init(&b);
  ssh_ed25519_blob(&b, pk);
  sha256(b.d, b.len, h);
  buf_free(&b);
  strcpy(out, "SHA256:");
  b64_encode(h, 32, out + 7, 0);
}
