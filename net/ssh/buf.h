/* growable byte buffers with SSH wire encoding (RFC 4251 section 5) */
#ifndef BUF_H
#define BUF_H
#include <stddef.h>
#include <stdint.h>

struct buf {
  uint8_t *d;
  size_t len, cap;                      /* bytes held, allocated */
  size_t pos;                           /* read position */
  int err;                              /* a read ran past the end */
};

void buf_init(struct buf *b);
void buf_free(struct buf *b);
void buf_reset(struct buf *b);
void buf_consume(struct buf *b, size_t n);      /* drop n bytes at the front */
void buf_reserve(struct buf *b, size_t n);
size_t buf_left(const struct buf *b);           /* unread bytes */

void put_bytes(struct buf *b, const void *p, size_t n);
void put_u8(struct buf *b, uint8_t v);
void put_u32(struct buf *b, uint32_t v);
void put_u64(struct buf *b, uint64_t v);
void put_str(struct buf *b, const void *p, size_t n);
void put_cstr(struct buf *b, const char *s);
void put_mpint(struct buf *b, const uint8_t *be, size_t n);   /* unsigned, big endian */

uint8_t get_u8(struct buf *b);
uint32_t get_u32(struct buf *b);
uint64_t get_u64(struct buf *b);
const uint8_t *get_bytes(struct buf *b, size_t n);
const uint8_t *get_str(struct buf *b, uint32_t *len);
int get_cstr(struct buf *b, char *out, size_t room);  /* 0 if too long or bad */

uint32_t peek_u32(const uint8_t *p);
void poke_u32(uint8_t *p, uint32_t v);

/* base64 (RFC 4648); decode returns the length or -1 */
size_t b64_encode(const uint8_t *in, size_t n, char *out, int pad);
int b64_decode(const char *in, size_t n, uint8_t *out, size_t room);

/* name-list helpers: is `name` in the comma separated list? */
int namelist_has(const uint8_t *list, uint32_t len, const char *name);

#endif
