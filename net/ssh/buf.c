/* growable byte buffers with SSH wire encoding */
#include <stdlib.h>
#include <string.h>
#include "buf.h"

void buf_init(struct buf *b)
{
  memset(b, 0, sizeof(*b));
}

void buf_free(struct buf *b)
{
  if (b->d)
  {
    memset(b->d, 0, b->cap);            /* may have held secrets */
    free(b->d);
  }
  buf_init(b);
}

void buf_reset(struct buf *b)
{
  b->len = b->pos = 0;
  b->err = 0;
}

void buf_consume(struct buf *b, size_t n)
{
  if (n >= b->len)
  {
    b->len = 0;
    b->pos = 0;
    return;
  }
  memmove(b->d, b->d + n, b->len - n);
  b->len -= n;
  b->pos = b->pos > n ? b->pos - n : 0;
}

void buf_reserve(struct buf *b, size_t n)
{
  size_t cap;
  uint8_t *d;

  if (b->len + n <= b->cap)
    return;
  cap = b->cap ? b->cap : 256;
  while (cap < b->len + n)
    cap *= 2;
  d = malloc(cap);
  if (!d)
    abort();
  if (b->d)
  {
    memcpy(d, b->d, b->len);
    memset(b->d, 0, b->cap);
    free(b->d);
  }
  b->d = d;
  b->cap = cap;
}

size_t buf_left(const struct buf *b)
{
  return b->len - b->pos;
}

void put_bytes(struct buf *b, const void *p, size_t n)
{
  buf_reserve(b, n);
  memcpy(b->d + b->len, p, n);
  b->len += n;
}

void put_u8(struct buf *b, uint8_t v)
{
  put_bytes(b, &v, 1);
}

uint32_t peek_u32(const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void poke_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

void put_u32(struct buf *b, uint32_t v)
{
  uint8_t t[4];

  poke_u32(t, v);
  put_bytes(b, t, 4);
}

void put_u64(struct buf *b, uint64_t v)
{
  put_u32(b, (uint32_t)(v >> 32));
  put_u32(b, (uint32_t)v);
}

void put_str(struct buf *b, const void *p, size_t n)
{
  put_u32(b, (uint32_t)n);
  put_bytes(b, p, n);
}

void put_cstr(struct buf *b, const char *s)
{
  put_str(b, s, strlen(s));
}

void put_mpint(struct buf *b, const uint8_t *be, size_t n)
{
  while (n && !*be)
  {
    be++;
    n--;
  }
  if (n && (*be & 0x80))
  {
    put_u32(b, (uint32_t)n + 1);
    put_u8(b, 0);
    put_bytes(b, be, n);
  }
  else
    put_str(b, be, n);
}

const uint8_t *get_bytes(struct buf *b, size_t n)
{
  const uint8_t *p;

  if (b->err || n > b->len - b->pos)
  {
    b->err = 1;
    return NULL;
  }
  p = b->d + b->pos;
  b->pos += n;
  return p;
}

uint8_t get_u8(struct buf *b)
{
  const uint8_t *p = get_bytes(b, 1);
  return p ? *p : 0;
}

uint32_t get_u32(struct buf *b)
{
  const uint8_t *p = get_bytes(b, 4);
  return p ? peek_u32(p) : 0;
}

uint64_t get_u64(struct buf *b)
{
  uint64_t hi = get_u32(b);
  return hi << 32 | get_u32(b);
}

const uint8_t *get_str(struct buf *b, uint32_t *len)
{
  uint32_t n = get_u32(b);
  const uint8_t *p = get_bytes(b, n);

  *len = p ? n : 0;
  return p;
}

int get_cstr(struct buf *b, char *out, size_t room)
{
  uint32_t n;
  const uint8_t *p = get_str(b, &n);

  if (!p || n >= room || memchr(p, 0, n))
  {
    if (room)
      *out = 0;
    return 0;
  }
  memcpy(out, p, n);
  out[n] = 0;
  return 1;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t n, char *out, int pad)
{
  size_t i, o = 0;

  for (i = 0; i + 2 < n; i += 3)
  {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    out[o++] = b64[v >> 18];
    out[o++] = b64[(v >> 12) & 63];
    out[o++] = b64[(v >> 6) & 63];
    out[o++] = b64[v & 63];
  }
  if (i < n)
  {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
    out[o++] = b64[v >> 18];
    out[o++] = b64[(v >> 12) & 63];
    if (i + 1 < n)
      out[o++] = b64[(v >> 6) & 63];
    else if (pad)
      out[o++] = '=';
    if (pad)
      out[o++] = '=';
  }
  out[o] = 0;
  return o;
}

int b64_decode(const char *in, size_t n, uint8_t *out, size_t room)
{
  uint32_t v = 0;
  int bits = 0;
  size_t i, o = 0;

  for (i = 0; i < n && in[i] != '='; i++)
  {
    const char *c = strchr(b64, in[i]);
    if (!c || !in[i])
      return -1;
    v = v << 6 | (uint32_t)(c - b64);
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      if (o >= room)
        return -1;
      out[o++] = (uint8_t)(v >> bits);
    }
  }
  return (int)o;
}

int namelist_has(const uint8_t *list, uint32_t len, const char *name)
{
  size_t n = strlen(name);
  uint32_t i = 0;

  while (i <= len)
  {
    uint32_t j = i;
    while (j < len && list[j] != ',')
      j++;
    if (j - i == n && !memcmp(list + i, name, n))
      return 1;
    i = j + 1;
  }
  return 0;
}
