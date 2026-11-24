/* OpenSSH private key files ("openssh-key-v1", PROTOCOL.key), ed25519,
   without a passphrase: read and write */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/monocypher.h"
#include "rng.h"
#include "keyfile.h"

#define BEGIN "-----BEGIN OPENSSH PRIVATE KEY-----"
#define END "-----END OPENSSH PRIVATE KEY-----"
#define MAGIC "openssh-key-v1"

int key_read(const char *path, uint8_t sk[64], uint8_t pk[32], char *err, size_t errn)
{
  FILE *f = fopen(path, "r");
  char line[256], *b64;
  size_t len = 0, cap = 8192;
  uint8_t *raw;
  int n, in = 0, ok = 0;
  struct buf b, priv;
  uint32_t l1, l2, kn, pn, sn, i;
  const uint8_t *cipher, *kdf, *p, *kpk, *ksk;

  if (!f)
  {
    snprintf(err, errn, "cannot open %s", path);
    return 0;
  }
  b64 = malloc(cap);
  raw = malloc(cap);
  if (!b64 || !raw)
    abort();
  while (fgets(line, sizeof(line), f))
  {
    char *c;
    if (!strncmp(line, BEGIN, strlen(BEGIN)))
    {
      in = 1;
      continue;
    }
    if (!strncmp(line, END, strlen(END)))
      break;
    if (!in)
      continue;
    for (c = line; *c; c++)
      if (*c > ' ' && len + 1 < cap)
        b64[len++] = *c;
  }
  fclose(f);
  n = b64_decode(b64, len, raw, cap);
  free(b64);
  if (n <= (int)sizeof(MAGIC) || memcmp(raw, MAGIC, sizeof(MAGIC)))
  {
    snprintf(err, errn, "%s is not an OpenSSH private key", path);
    free(raw);
    return 0;
  }
  b.d = raw; b.len = (size_t)n; b.cap = 0; b.pos = sizeof(MAGIC); b.err = 0;
  cipher = get_str(&b, &l1);
  kdf = get_str(&b, &l2);
  get_str(&b, &kn);                     /* kdf options */
  if (b.err || l1 != 4 || memcmp(cipher, "none", 4) || l2 != 4 || memcmp(kdf, "none", 4))
  {
    snprintf(err, errn, "%s has a passphrase (not supported: make one without)", path);
    goto out;
  }
  if (get_u32(&b) != 1)
  {
    snprintf(err, errn, "%s: one key per file please", path);
    goto out;
  }
  get_str(&b, &pn);                     /* public key blob */
  p = get_str(&b, &sn);
  if (b.err)
    goto bad;
  priv.d = (uint8_t *)p; priv.len = sn; priv.cap = 0; priv.pos = 0; priv.err = 0;
  if (get_u32(&priv) != get_u32(&priv))
    goto bad;                           /* check integers */
  p = get_str(&priv, &l1);
  if (priv.err || l1 != 11 || memcmp(p, "ssh-ed25519", 11))
  {
    snprintf(err, errn, "%s: only ed25519 keys are supported", path);
    goto out;
  }
  kpk = get_str(&priv, &l1);
  ksk = get_str(&priv, &l2);
  if (priv.err || l1 != 32 || l2 != 64)
    goto bad;
  memcpy(pk, kpk, 32);
  memcpy(sk, ksk, 64);
  for (i = 0; i < 32 && sk[32 + i] == pk[i]; i++)
    ;
  if (i != 32)
    goto bad;
  ok = 1;
  goto out;
bad:
  snprintf(err, errn, "%s is damaged", path);
out:
  crypto_wipe(raw, cap);
  free(raw);
  return ok;
}

int key_write(const char *path, const uint8_t sk[64], const uint8_t pk[32], const char *comment)
{
  struct buf b, priv, pub;
  uint8_t check[4];
  char *b64;
  size_t n, i;
  FILE *f;
  int pad;

  buf_init(&b);
  buf_init(&priv);
  buf_init(&pub);
  put_cstr(&pub, "ssh-ed25519");
  put_str(&pub, pk, 32);
  rng_bytes(check, 4);
  put_bytes(&priv, check, 4);
  put_bytes(&priv, check, 4);
  put_cstr(&priv, "ssh-ed25519");
  put_str(&priv, pk, 32);
  put_str(&priv, sk, 64);
  put_cstr(&priv, comment);
  for (pad = 1; priv.len % 8; pad++)
    put_u8(&priv, (uint8_t)pad);
  put_bytes(&b, MAGIC, sizeof(MAGIC));
  put_cstr(&b, "none");
  put_cstr(&b, "none");
  put_cstr(&b, "");
  put_u32(&b, 1);
  put_str(&b, pub.d, pub.len);
  put_str(&b, priv.d, priv.len);
  b64 = malloc(b.len * 2 + 4);
  if (!b64)
    abort();
  n = b64_encode(b.d, b.len, b64, 1);
  f = fopen(path, "w");
  if (f)
  {
    fprintf(f, "%s\n", BEGIN);
    for (i = 0; i < n; i += 70)
      fprintf(f, "%.70s\n", b64 + i);
    fprintf(f, "%s\n", END);
    fclose(f);
  }
  crypto_wipe(b64, b.len * 2 + 4);
  free(b64);
  buf_free(&b);
  buf_free(&priv);
  buf_free(&pub);
  return f != NULL;
}

void key_public_line(const uint8_t pk[32], const char *comment, char *out, size_t n)
{
  struct buf b;
  char b64[128];

  buf_init(&b);
  put_cstr(&b, "ssh-ed25519");
  put_str(&b, pk, 32);
  b64_encode(b.d, b.len, b64, 1);
  buf_free(&b);
  snprintf(out, n, "ssh-ed25519 %s %s", b64, comment);
}
