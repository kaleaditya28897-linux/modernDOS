#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdint.h>
#include "buf.h"

#define CON_COLS 80
#define CON_ROWS 25

struct console {
  uint8_t last[CON_COLS * CON_ROWS * 2];  /* what the terminal shows */
  int have, row, col, hidden, attr;
  int last_cr;                            /* input: CR seen (CR LF = 1 Enter) */
  size_t used;                            /* input bytes con_keys consumed */
};

void con_init(struct console *c);
/* ANSI output for the changes since the last call; 1 if any */
int con_render(struct console *c, const uint8_t *cells, int row, int col, int hidden, struct buf *o);
/* leave the terminal in a sane state */
void con_reset(struct buf *o);
/* terminal input -> BIOS keys (scan << 8 | ASCII); c->used = bytes taken */
int con_keys(struct console *c, const uint8_t *in, size_t n, uint16_t *keys, int max);

/* code page 437 <-> Unicode */
uint8_t cp437_from_unicode(uint32_t u);
uint32_t cp437_to_unicode(uint8_t c);

#endif
