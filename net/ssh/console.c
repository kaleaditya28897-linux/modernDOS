/* remote console: a DOS text screen (80x25, CGA attributes, code page 437)
   to ANSI/UTF-8 terminal output, and terminal input to BIOS keys */
#include <string.h>
#include <stdio.h>
#include "console.h"

static const uint16_t cp437[256] = {
  0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
  0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
  0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
  0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
  0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
  0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
  0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
  0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x2302,
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
  0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
  0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
  0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
  0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
  0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
  0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
  0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
  0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
  0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

static void put(struct buf *o, const char *s)
{
  put_bytes(o, s, strlen(s));
}

static void put_char(struct buf *o, uint8_t c)
{
  uint16_t u = cp437[c];
  uint8_t t[3];

  if (u < 0x80)
    put_u8(o, (uint8_t)u);
  else if (u < 0x800)
  {
    t[0] = (uint8_t)(0xc0 | u >> 6);
    t[1] = (uint8_t)(0x80 | (u & 0x3f));
    put_bytes(o, t, 2);
  }
  else
  {
    t[0] = (uint8_t)(0xe0 | u >> 12);
    t[1] = (uint8_t)(0x80 | ((u >> 6) & 0x3f));
    t[2] = (uint8_t)(0x80 | (u & 0x3f));
    put_bytes(o, t, 3);
  }
}

static void set_attr(struct console *c, struct buf *o, uint8_t a)
{
  static const char ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };   /* CGA -> ANSI order */
  char s[24];
  int fg = a & 15, bg = (a >> 4) & 7;

  if (c->attr == a)
    return;
  snprintf(s, sizeof(s), "\033[0;%d;%dm", fg >= 8 ? 90 + ansi[fg - 8] : 30 + ansi[fg], 40 + ansi[bg]);
  put(o, s);
  c->attr = a;
}

static void goto_rc(struct buf *o, int r, int col)
{
  char s[32];

  snprintf(s, sizeof(s), "\033[%d;%dH", r + 1, col + 1);
  put(o, s);
}

void con_init(struct console *c)
{
  memset(c, 0, sizeof(*c));
  c->attr = -1;
}

static int row_eq(const uint8_t *a, int ra, const uint8_t *b, int rb)
{
  return !memcmp(a + ra * CON_COLS * 2, b + rb * CON_COLS * 2, CON_COLS * 2);
}

static int row_blank(const uint8_t *a, int r)
{
  int i;

  for (i = 0; i < CON_COLS; i++)
    if (a[(r * CON_COLS + i) * 2] > ' ')
      return 0;
  return 1;
}

int con_render(struct console *c, const uint8_t *cells, int row, int col, int hidden, struct buf *o)
{
  size_t start = o->len;
  int r, i, k;

  if (!c->have)
  {
    /* fresh terminal: 25 line scrolling region, clear, draw all */
    put(o, "\033[0m\033[1;25r\033[2J");
    c->attr = -1;
    memset(c->last, 0, sizeof(c->last));
    for (i = 0; i < CON_COLS * CON_ROWS; i++)
      c->last[i * 2 + 1] = 0x07;
    c->row = c->col = -1;
    c->hidden = -1;
    c->have = 1;
  }
  else if (!memcmp(c->last, cells, sizeof(c->last)) && row == c->row && col == c->col &&
           hidden == c->hidden)
    return 0;

  /* scrolled up by k lines? then let the terminal scroll too */
  for (k = 1; k < CON_ROWS; k++)
  {
    int ok = 1, nonblank = 0;
    for (r = 0; r + k < CON_ROWS && ok; r++)
    {
      ok = row_eq(cells, r, c->last, r + k);
      nonblank |= !row_blank(cells, r);
    }
    if (ok && nonblank)
      break;
  }
  if (k < CON_ROWS && memcmp(c->last, cells, sizeof(c->last)))
  {
    goto_rc(o, CON_ROWS - 1, 0);
    set_attr(c, o, 0x07);
    for (i = 0; i < k; i++)
      put(o, "\n");
    memmove(c->last, c->last + k * CON_COLS * 2, (size_t)(CON_ROWS - k) * CON_COLS * 2);
    for (i = (CON_ROWS - k) * CON_COLS; i < CON_ROWS * CON_COLS; i++)
    {
      c->last[i * 2] = ' ';
      c->last[i * 2 + 1] = 0x07;
    }
  }

  /* changed cells, row by row */
  for (r = 0; r < CON_ROWS; r++)
  {
    int first = -1, last = -1;
    for (i = 0; i < CON_COLS; i++)
    {
      int p = (r * CON_COLS + i) * 2;
      uint8_t ch = cells[p] ? cells[p] : ' ';
      uint8_t lch = c->last[p] ? c->last[p] : ' ';
      if (ch != lch || cells[p + 1] != c->last[p + 1])
      {
        if (first < 0)
          first = i;
        last = i;
      }
    }
    if (first < 0)
      continue;
    if (r == CON_ROWS - 1 && last == CON_COLS - 1)
      last--;                           /* never the last cell: it would scroll */
    goto_rc(o, r, first);
    for (i = first; i <= last; i++)
    {
      int p = (r * CON_COLS + i) * 2;
      set_attr(c, o, cells[p + 1]);
      put_char(o, cells[p]);
    }
  }
  memcpy(c->last, cells, sizeof(c->last));

  if (hidden != c->hidden)
  {
    put(o, hidden ? "\033[?25l" : "\033[?25h");
    c->hidden = hidden;
  }
  if (row >= 0 && row < CON_ROWS && col >= 0 && col < CON_COLS)
    goto_rc(o, row, col);
  c->row = row;
  c->col = col;
  return o->len > start;
}

void con_reset(struct buf *o)
{
  put(o, "\033[0m\033[r\033[?25h\033[25;1H\r\n");
}

/* ---------------------------------------------------------------- */
/* input                                                            */

/* scan codes of the US keyboard for ASCII 20h..7Eh */
static const uint8_t scan_of[95] = {
  0x39, 0x02, 0x28, 0x04, 0x05, 0x06, 0x08, 0x28, 0x0A, 0x0B, 0x09, 0x0D, 0x33, 0x0C, 0x34, 0x35,
  0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x27, 0x27, 0x33, 0x0D, 0x34, 0x35,
  0x03, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
  0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x07, 0x0C,
  0x29, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
  0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x29
};

static uint16_t key_ascii(uint8_t c)
{
  if (c == '\r')
    return 0x1c0d;
  if (c == 0x7f || c == 0x08)
    return 0x0e08;
  if (c == '\t')
    return 0x0f09;
  if (c == 0x1b)
    return 0x011b;
  if (c >= 1 && c <= 26)                /* Ctrl+letter */
    return (uint16_t)(scan_of['a' - 0x20 + c - 1] << 8 | c);
  if (c >= 0x20 && c <= 0x7e)
    return (uint16_t)(scan_of[c - 0x20] << 8 | c);
  return c;
}

static uint16_t key_csi_num(int n)
{
  switch (n)
  {
    case 1: case 7: return 0x4700;      /* Home */
    case 2: return 0x5200;              /* Insert */
    case 3: return 0x5300;              /* Delete */
    case 4: case 8: return 0x4f00;      /* End */
    case 5: return 0x4900;              /* Page Up */
    case 6: return 0x5100;              /* Page Down */
    case 11: return 0x3b00; case 12: return 0x3c00; case 13: return 0x3d00;
    case 14: return 0x3e00; case 15: return 0x3f00;
    case 17: return 0x4000; case 18: return 0x4100; case 19: return 0x4200;
    case 20: return 0x4300; case 21: return 0x4400;
    case 23: return 0x8500; case 24: return 0x8600;
  }
  return 0;
}

static uint16_t key_final(uint8_t f)
{
  switch (f)
  {
    case 'A': return 0x4800;            /* arrows */
    case 'B': return 0x5000;
    case 'C': return 0x4d00;
    case 'D': return 0x4b00;
    case 'H': return 0x4700;
    case 'F': return 0x4f00;
    case 'P': return 0x3b00;            /* F1..F4 (SS3 or CSI 1;mP) */
    case 'Q': return 0x3c00;
    case 'R': return 0x3d00;
    case 'S': return 0x3e00;
  }
  return 0;
}

/* back from Unicode to code page 437 (0 if there is no such character) */
uint8_t cp437_from_unicode(uint32_t u)
{
  int i;

  if (u >= 0x20 && u < 0x7f)
    return (uint8_t)u;
  for (i = 128; i < 256; i++)
    if (cp437[i] == u)
      return (uint8_t)i;
  return 0;
}

uint32_t cp437_to_unicode(uint8_t c)
{
  return cp437[c];
}

int con_keys(struct console *c, const uint8_t *in, size_t n, uint16_t *keys, int max)
{
  size_t i = 0;
  int k = 0;

  while (i < n && k < max)
  {
    uint8_t ch = in[i];
    if (ch == '\n' && c->last_cr)
    {
      c->last_cr = 0;
      i++;
      continue;                         /* CR LF: one Enter */
    }
    c->last_cr = ch == '\r';
    if (ch == '\n')
      ch = '\r';
    if (ch == 0x1b && i + 1 < n && (in[i + 1] == '[' || in[i + 1] == 'O'))
    {
      size_t j = i + 2;
      int num = 0;
      uint16_t key;
      while (j < n && ((in[j] >= '0' && in[j] <= '9') || in[j] == ';'))
      {
        if (in[j] == ';')
          num = num ? num : 1;          /* keep the first parameter */
        else if (!memchr(in + i + 2, ';', j - i - 2))
          num = num * 10 + (in[j] - '0');
        j++;
      }
      if (j >= n)
        break;                          /* incomplete: wait for the rest */
      key = in[j] == '~' ? key_csi_num(num) : key_final(in[j]);
      if (key)
        keys[k++] = key;
      i = j + 1;
      continue;
    }
    if (ch >= 0xc0)                     /* UTF-8 sequence */
    {
      size_t len = ch >= 0xf0 ? 4 : ch >= 0xe0 ? 3 : 2, j;
      uint32_t u = ch & (0x3f >> (len - 1));
      if (i + len > n)
        break;
      for (j = 1; j < len; j++)
        u = u << 6 | (in[i + j] & 0x3f);
      ch = cp437_from_unicode(u);
      if (ch)
        keys[k++] = ch;
      i += len;
      continue;
    }
    keys[k++] = key_ascii(ch);
    i++;
  }
  c->used = i;
  return k;
}
