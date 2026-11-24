/* terminal emulator for the SSH client (see term.h) */
#include <string.h>
#include <stdio.h>
#include "console.h"
#include "term.h"

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET };

static const uint8_t cga_of_ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static uint8_t attr(const struct term *t)
{
  int fg = t->fg, bg = t->bg & 7;

  if (t->bold && fg < 8)
    fg += 8;
  if (t->reverse)
  {
    int tmp = fg & 7;
    fg = bg | (fg & 8);
    bg = tmp;
  }
  return (uint8_t)(cga_of_ansi[bg] << 4 | (fg & 8) | cga_of_ansi[fg & 7]);
}

static uint16_t blank(const struct term *t)
{
  return (uint16_t)(attr(t) << 8 | ' ');
}

static void clear_cells(struct term *t, int row, int c0, int c1)
{
  int c;

  for (c = c0; c < c1 && c < TERM_COLS; c++)
    t->cell[row][c] = blank(t);
  t->dirty = 1;
}

/* scroll rows top..bottom up by n (down if n < 0) */
static void scroll(struct term *t, int top, int bottom, int n)
{
  int r, h = bottom - top + 1;

  if (n > h)
    n = h;
  if (n < -h)
    n = -h;
  if (n > 0)
  {
    memmove(t->cell[top], t->cell[top + n], (size_t)(h - n) * sizeof(t->cell[0]));
    for (r = bottom - n + 1; r <= bottom; r++)
      clear_cells(t, r, 0, TERM_COLS);
  }
  else if (n < 0)
  {
    n = -n;
    memmove(t->cell[top + n], t->cell[top], (size_t)(h - n) * sizeof(t->cell[0]));
    for (r = top; r < top + n; r++)
      clear_cells(t, r, 0, TERM_COLS);
  }
  t->dirty = 1;
}

static void line_feed(struct term *t)
{
  if (t->row == t->bottom)
    scroll(t, t->top, t->bottom, 1);
  else if (t->row < TERM_ROWS - 1)
    t->row++;
}

static void reset(struct term *t)
{
  struct buf *reply = t->reply;
  int r;

  memset(t, 0, sizeof(*t));
  t->reply = reply;
  t->fg = 7;
  t->bottom = TERM_ROWS - 1;
  t->cursor_on = 1;
  t->autowrap = 1;
  for (r = 0; r < TERM_ROWS; r++)
    clear_cells(t, r, 0, TERM_COLS);
}

void term_init(struct term *t, struct buf *reply)
{
  t->reply = reply;
  reset(t);
}

static void put_glyph(struct term *t, uint8_t ch)
{
  if (t->wrap_pending)
  {
    t->col = 0;
    line_feed(t);
    t->wrap_pending = 0;
  }
  t->cell[t->row][t->col] = (uint16_t)(attr(t) << 8 | ch);
  t->dirty = 1;
  if (t->col == TERM_COLS - 1)
    t->wrap_pending = t->autowrap;
  else
    t->col++;
}

/* DEC special graphics (ESC ( 0) to code page 437 */
static uint8_t dec_graphics(uint8_t c)
{
  switch (c)
  {
    case 'j': return 0xd9; case 'k': return 0xbf; case 'l': return 0xda; case 'm': return 0xc0;
    case 'n': return 0xc5; case 'q': return 0xc4; case 't': return 0xc3; case 'u': return 0xb4;
    case 'v': return 0xc1; case 'w': return 0xc2; case 'x': return 0xb3; case 'a': return 0xb1;
    case '`': return 0x04; case 'f': return 0xf8; case 'g': return 0xf1; case '~': return 0xfa;
    case 'y': return 0xf3; case 'z': return 0xf2; case '0': return 0xdb;
  }
  return c;
}

static int par(const struct term *t, int i, int def)
{
  return i < t->nparams && t->params[i] > 0 ? t->params[i] : def;
}

static void clamp(struct term *t)
{
  if (t->row < 0) t->row = 0;
  if (t->row >= TERM_ROWS) t->row = TERM_ROWS - 1;
  if (t->col < 0) t->col = 0;
  if (t->col >= TERM_COLS) t->col = TERM_COLS - 1;
  t->wrap_pending = 0;
  t->dirty = 1;
}

/* 256-colour / RGB to the nearest of the 16 ANSI colours */
static int nearest16(int r, int g, int b)
{
  static const uint8_t pal[16][3] = {
    {0,0,0},{170,0,0},{0,170,0},{170,85,0},{0,0,170},{170,0,170},{0,170,170},{170,170,170},
    {85,85,85},{255,85,85},{85,255,85},{255,255,85},{85,85,255},{255,85,255},{85,255,255},{255,255,255}
  };
  int i, best = 0;
  long bd = 1L << 30;

  for (i = 0; i < 16; i++)
  {
    long d = (long)(r - pal[i][0]) * (r - pal[i][0]) + (long)(g - pal[i][1]) * (g - pal[i][1]) +
             (long)(b - pal[i][2]) * (b - pal[i][2]);
    if (d < bd)
    {
      bd = d;
      best = i;
    }
  }
  return best;
}

static int color256(int n)
{
  if (n < 16)
    return n;
  if (n >= 232)
  {
    int v = 8 + (n - 232) * 10;
    return nearest16(v, v, v);
  }
  n -= 16;
  return nearest16((n / 36) ? (n / 36) * 40 + 55 : 0, ((n / 6) % 6) ? ((n / 6) % 6) * 40 + 55 : 0,
                   (n % 6) ? (n % 6) * 40 + 55 : 0);
}

static void sgr(struct term *t)
{
  int i;

  if (!t->nparams)
    t->nparams = 1, t->params[0] = 0;
  for (i = 0; i < t->nparams; i++)
  {
    int p = t->params[i];
    if (p == 0)
    {
      t->fg = 7;
      t->bg = 0;
      t->bold = t->reverse = 0;
    }
    else if (p == 1) t->bold = 1;
    else if (p == 22) t->bold = 0;
    else if (p == 7) t->reverse = 1;
    else if (p == 27) t->reverse = 0;
    else if (p >= 30 && p <= 37) t->fg = (uint8_t)(p - 30);
    else if (p == 39) t->fg = 7;
    else if (p >= 40 && p <= 47) t->bg = (uint8_t)(p - 40);
    else if (p == 49) t->bg = 0;
    else if (p >= 90 && p <= 97) t->fg = (uint8_t)(p - 90 + 8);
    else if (p >= 100 && p <= 107) t->bg = (uint8_t)(p - 100);
    else if ((p == 38 || p == 48) && i + 1 < t->nparams)
    {
      int c = -1;
      if (t->params[i + 1] == 5 && i + 2 < t->nparams)
      {
        c = color256(t->params[i + 2]);
        i += 2;
      }
      else if (t->params[i + 1] == 2 && i + 4 < t->nparams)
      {
        c = nearest16(t->params[i + 2], t->params[i + 3], t->params[i + 4]);
        i += 4;
      }
      if (c >= 0)
      {
        if (p == 38)
          t->fg = (uint8_t)c;
        else
          t->bg = (uint8_t)(c & 7);
      }
    }
  }
}

static void reply(struct term *t, const char *s)
{
  if (t->reply)
    put_bytes(t->reply, s, strlen(s));
}

static void set_mode(struct term *t, int on)
{
  int i;

  for (i = 0; i < t->nparams; i++)
  {
    int p = t->params[i];
    if (!t->priv)
      continue;                         /* ANSI modes (insert ...) not supported */
    if (p == 1)
      t->app_cursor = on;
    else if (p == 7)
      t->autowrap = on;
    else if (p == 25)
    {
      t->cursor_on = on;
      t->dirty = 1;
    }
    else if ((p == 47 || p == 1047 || p == 1049) && on != t->alt)
    {
      int r;
      if (on)
      {
        if (p == 1049)
          t->srow = t->row, t->scol = t->col;
        memcpy(t->saved, t->cell, sizeof(t->cell));
        for (r = 0; r < TERM_ROWS; r++)
          clear_cells(t, r, 0, TERM_COLS);
      }
      else
      {
        memcpy(t->cell, t->saved, sizeof(t->cell));
        if (p == 1049)
          t->row = t->srow, t->col = t->scol;
      }
      t->alt = on;
      t->dirty = 1;
    }
  }
}

static void csi(struct term *t, uint8_t f)
{
  int n = par(t, 0, 1), r;

  switch (f)
  {
    case 'A': t->row -= n; if (t->row < t->top && t->row + n >= t->top) t->row = t->top; clamp(t); break;
    case 'B': t->row += n; if (t->row > t->bottom && t->row - n <= t->bottom) t->row = t->bottom; clamp(t); break;
    case 'C': t->col += n; clamp(t); break;
    case 'D': t->col -= n; clamp(t); break;
    case 'E': t->row += n; t->col = 0; clamp(t); break;
    case 'F': t->row -= n; t->col = 0; clamp(t); break;
    case 'G': case '`': t->col = n - 1; clamp(t); break;
    case 'd': t->row = n - 1; clamp(t); break;
    case 'H': case 'f': t->row = par(t, 0, 1) - 1; t->col = par(t, 1, 1) - 1; clamp(t); break;
    case 'J':
    {
      int m = t->nparams ? t->params[0] : 0;
      if (m == 0)
      {
        clear_cells(t, t->row, t->col, TERM_COLS);
        for (r = t->row + 1; r < TERM_ROWS; r++)
          clear_cells(t, r, 0, TERM_COLS);
      }
      else if (m == 1)
      {
        clear_cells(t, t->row, 0, t->col + 1);
        for (r = 0; r < t->row; r++)
          clear_cells(t, r, 0, TERM_COLS);
      }
      else
        for (r = 0; r < TERM_ROWS; r++)
          clear_cells(t, r, 0, TERM_COLS);
      break;
    }
    case 'K':
    {
      int m = t->nparams ? t->params[0] : 0;
      clear_cells(t, t->row, m == 0 ? t->col : 0, m == 1 ? t->col + 1 : TERM_COLS);
      break;
    }
    case 'L':
      if (t->row >= t->top && t->row <= t->bottom)
        scroll(t, t->row, t->bottom, -n);
      break;
    case 'M':
      if (t->row >= t->top && t->row <= t->bottom)
        scroll(t, t->row, t->bottom, n);
      break;
    case 'P':
      if (n > TERM_COLS - t->col)
        n = TERM_COLS - t->col;
      memmove(&t->cell[t->row][t->col], &t->cell[t->row][t->col + n],
              (size_t)(TERM_COLS - t->col - n) * 2);
      clear_cells(t, t->row, TERM_COLS - n, TERM_COLS);
      break;
    case '@':
      if (n > TERM_COLS - t->col)
        n = TERM_COLS - t->col;
      memmove(&t->cell[t->row][t->col + n], &t->cell[t->row][t->col],
              (size_t)(TERM_COLS - t->col - n) * 2);
      clear_cells(t, t->row, t->col, t->col + n);
      break;
    case 'X': clear_cells(t, t->row, t->col, t->col + n); break;
    case 'S': scroll(t, t->top, t->bottom, n); break;
    case 'T': scroll(t, t->top, t->bottom, -n); break;
    case 'm': sgr(t); break;
    case 'r':
    {
      int top = par(t, 0, 1) - 1, bottom = par(t, 1, TERM_ROWS) - 1;
      if (bottom >= TERM_ROWS)
        bottom = TERM_ROWS - 1;
      if (top < bottom)
      {
        t->top = top;
        t->bottom = bottom;
        t->row = t->col = 0;
        clamp(t);
      }
      break;
    }
    case 's': t->srow = t->row; t->scol = t->col; break;
    case 'u': t->row = t->srow; t->col = t->scol; clamp(t); break;
    case 'h': set_mode(t, 1); break;
    case 'l': set_mode(t, 0); break;
    case 'n':
      if (par(t, 0, 0) == 6)
      {
        char s[40];
        snprintf(s, sizeof(s), "\033[%d;%dR", t->row + 1, t->col + 1);
        reply(t, s);
      }
      else if (par(t, 0, 0) == 5)
        reply(t, "\033[0n");
      break;
    case 'c':
      if (!t->priv)
        reply(t, "\033[?1;2c");
      break;
  }
}

static void control(struct term *t, uint8_t c)
{
  switch (c)
  {
    case '\r': t->col = 0; t->wrap_pending = 0; break;
    case '\n': case 0x0b: case 0x0c: line_feed(t); t->wrap_pending = 0; break;
    case '\b': if (t->col > 0) t->col--; t->wrap_pending = 0; break;
    case '\t':
      t->col = (t->col / 8 + 1) * 8;
      if (t->col >= TERM_COLS)
        t->col = TERM_COLS - 1;
      break;
    case 0x0e: case 0x0f: break;        /* SO/SI: G1 not supported */
  }
  t->dirty = 1;
}

void term_write(struct term *t, const uint8_t *p, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
  {
    uint8_t c = p[i];
    switch (t->state)
    {
      case S_GROUND:
        if (c == 0x1b)
        {
          t->state = S_ESC;
          t->utf_left = 0;
        }
        else if (c < 0x20)
          control(t, c);
        else if (c < 0x80)
        {
          t->utf_left = 0;
          put_glyph(t, t->g0_lines ? dec_graphics(c) : c);
        }
        else if (c >= 0xc0)             /* UTF-8 lead byte */
        {
          t->utf_left = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
          t->utf = c & (0x3f >> t->utf_left);
        }
        else if (t->utf_left)           /* continuation */
        {
          t->utf = t->utf << 6 | (c & 0x3f);
          if (!--t->utf_left)
          {
            uint8_t g = cp437_from_unicode(t->utf);
            put_glyph(t, g ? g : '?');
          }
        }
        break;
      case S_ESC:
        t->state = S_GROUND;
        switch (c)
        {
          case '[':
            t->state = S_CSI;
            t->nparams = 0;
            t->priv = 0;
            memset(t->params, 0, sizeof(t->params));
            break;
          case ']': t->state = S_OSC; break;
          case '(': t->state = S_CHARSET; t->priv = 0; break;
          case ')': case '*': case '+': t->state = S_CHARSET; t->priv = -1; break;
          case '7': t->srow = t->row; t->scol = t->col; break;
          case '8': t->row = t->srow; t->col = t->scol; clamp(t); break;
          case 'D': line_feed(t); break;
          case 'E': t->col = 0; line_feed(t); break;
          case 'M':
            if (t->row == t->top)
              scroll(t, t->top, t->bottom, -1);
            else if (t->row > 0)
              t->row--;
            break;
          case 'c': reset(t); break;
        }
        break;
      case S_CHARSET:
        if (t->priv != -1)
          t->g0_lines = c == '0';
        t->priv = 0;
        t->state = S_GROUND;
        break;
      case S_CSI:
        if (c >= '0' && c <= '9')
        {
          if (!t->nparams)
            t->nparams = 1;
          if (t->params[t->nparams - 1] < 10000)
            t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (c - '0');
        }
        else if (c == ';' || c == ':')
        {
          if (!t->nparams)
            t->nparams = 1;
          if (t->nparams < 16)
            t->nparams++;
        }
        else if (c == '?' || c == '>' || c == '=')
          t->priv = 1;
        else if (c >= 0x40 && c <= 0x7e)
        {
          t->state = S_GROUND;
          csi(t, c);
        }
        else if (c < 0x20)
          control(t, c);
        break;
      case S_OSC:                       /* window titles etc.: ignored */
        if (c == 0x07)
          t->state = S_GROUND;
        else if (c == 0x1b)
          t->state = S_OSC_ESC;
        break;
      case S_OSC_ESC:
        t->state = c == '\\' ? S_GROUND : S_OSC;
        break;
    }
  }
}

void term_key(struct term *t, uint16_t key, struct buf *out)
{
  uint8_t ascii = (uint8_t)key, scan = (uint8_t)(key >> 8);
  const char *seq = NULL;
  char tmp[8];

  if (ascii && ascii != 0xe0)
  {
    if (ascii == 0x08)
      ascii = 0x7f;                     /* Backspace: DEL, as xterm */
    if (ascii >= 0x80)                  /* CP437 -> UTF-8 */
    {
      uint32_t u = cp437_to_unicode(ascii);
      if (u < 0x800)
      {
        tmp[0] = (char)(0xc0 | u >> 6);
        tmp[1] = (char)(0x80 | (u & 0x3f));
        put_bytes(out, tmp, 2);
      }
      else
      {
        tmp[0] = (char)(0xe0 | u >> 12);
        tmp[1] = (char)(0x80 | ((u >> 6) & 0x3f));
        tmp[2] = (char)(0x80 | (u & 0x3f));
        put_bytes(out, tmp, 3);
      }
      return;
    }
    put_u8(out, ascii);
    return;
  }
  switch (scan)
  {
    case 0x48: seq = t->app_cursor ? "\033OA" : "\033[A"; break;
    case 0x50: seq = t->app_cursor ? "\033OB" : "\033[B"; break;
    case 0x4d: seq = t->app_cursor ? "\033OC" : "\033[C"; break;
    case 0x4b: seq = t->app_cursor ? "\033OD" : "\033[D"; break;
    case 0x47: seq = t->app_cursor ? "\033OH" : "\033[H"; break;
    case 0x4f: seq = t->app_cursor ? "\033OF" : "\033[F"; break;
    case 0x49: seq = "\033[5~"; break;
    case 0x51: seq = "\033[6~"; break;
    case 0x52: seq = "\033[2~"; break;
    case 0x53: seq = "\033[3~"; break;
    case 0x3b: seq = "\033OP"; break;
    case 0x3c: seq = "\033OQ"; break;
    case 0x3d: seq = "\033OR"; break;
    case 0x3e: seq = "\033OS"; break;
    case 0x3f: seq = "\033[15~"; break;
    case 0x40: seq = "\033[17~"; break;
    case 0x41: seq = "\033[18~"; break;
    case 0x42: seq = "\033[19~"; break;
    case 0x43: seq = "\033[20~"; break;
    case 0x44: seq = "\033[21~"; break;
    case 0x85: seq = "\033[23~"; break;
    case 0x86: seq = "\033[24~"; break;
    case 0x03: seq = ""; put_u8(out, 0); break;   /* Ctrl+2: NUL */
  }
  if (seq)
    put_bytes(out, seq, strlen(seq));
}
