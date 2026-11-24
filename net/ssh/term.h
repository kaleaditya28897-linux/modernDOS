/* terminal emulator for the SSH client: a subset of xterm (VT100/ANSI)
   on an 80x25 text screen of CP437 cells with CGA attributes */
#ifndef TERM_H
#define TERM_H
#include <stdint.h>
#include "buf.h"

#define TERM_COLS 80
#define TERM_ROWS 25

struct term {
  uint16_t cell[TERM_ROWS][TERM_COLS];  /* attribute << 8 | character */
  uint16_t saved[TERM_ROWS][TERM_COLS]; /* main screen while the alternate shows */
  int row, col, wrap_pending;
  int top, bottom;                      /* scrolling region */
  uint8_t fg, bg;                       /* ANSI colours 0..15 */
  int bold, reverse;
  int cursor_on, app_cursor, autowrap, alt;
  int g0_lines;                         /* DEC special graphics in G0 */
  int srow, scol;                       /* saved cursor */
  int state;                            /* parser */
  int params[16], nparams, priv;
  uint32_t utf, utf_left;               /* UTF-8 decoder */
  int dirty;
  struct buf *reply;                    /* answers (DSR, DA) for the host */
};

void term_init(struct term *t, struct buf *reply);
void term_write(struct term *t, const uint8_t *p, size_t n);
/* a key from the BIOS (scan << 8 | ASCII) -> bytes for the host */
void term_key(struct term *t, uint16_t key, struct buf *out);

#endif
