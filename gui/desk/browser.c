/*
 * MDDESK Browser - a small web browser (M6).
 *
 * http and https (TLS 1.2, BearSSL), text layout: a page becomes lines of character
 * cells with a style (text, bold, heading, link, preformatted). Handles
 * headings, paragraphs, line breaks, lists, links, preformatted text,
 * rules, tables (cells in a row), image and form placeholders, character
 * entities and UTF-8 (shown in code page 437). Scripts and style sheets
 * are skipped.
 *
 * Address field + Go, Back; in the page: click a link, Up/Down, PgUp/PgDn,
 * Home/End scroll, Backspace goes back. The network is Watt-32 through
 * the packet driver (load PKTDRV before the desktop).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "desk.h"
#include "../../net/pkg/http.h"
#include "../../net/ssh/console.h"

enum { ST_TEXT, ST_BOLD, ST_HEAD, ST_LINK, ST_PRE, ST_DIM };
#define MAX_COLS  150
#define MAX_LINES 20000
#define MAX_LINKS 2000
#define MAX_PAGE  (2 * 1024 * 1024)
#define HISTORY   16

struct cell {
  unsigned char ch, style;
  unsigned short link;                  /* 1-based, 0: none */
};

struct line {
  struct cell *c;
  int n;
};

struct browser {
  struct window *win;
  struct ui *ui;
  struct widget *addr, *view, *status;
  struct line *lines;
  int nlines, cap;
  char **links;
  int nlinks;
  char title[80], url[512];
  int top;                              /* first line shown */
  int cols, rows;
  char history[HISTORY][512];
  int nhist;
  /* layout state while a page is parsed */
  struct cell cur[MAX_COLS];
  int ncur, style, link, pre, indent, pending_space, blank;
};

/* strstr without regard to case (not in every C library) */
static const char *find_nocase(const char *hay, const char *needle)
{
  size_t n = strlen(needle);

  for (; *hay; hay++)
    if (!strncasecmp(hay, needle, n))
      return hay;
  return 0;
}

static struct browser *the;             /* one browser window */
static int net_up;

/* ---------------------------------------------------------------- */
/* the page: lines of cells                                         */

static void page_clear(struct browser *b)
{
  int i;

  for (i = 0; i < b->nlines; i++)
    free(b->lines[i].c);
  for (i = 0; i < b->nlinks; i++)
    free(b->links[i]);
  b->nlines = b->nlinks = 0;
  b->ncur = b->style = b->link = b->pre = b->indent = b->pending_space = 0;
  b->blank = 1;
  b->top = 0;
  b->title[0] = 0;
}

static void flush_line(struct browser *b)
{
  struct line *l;

  if (b->nlines == MAX_LINES)
  {
    b->ncur = 0;
    return;
  }
  if (b->nlines == b->cap)
  {
    int cap = b->cap ? b->cap * 2 : 256;
    struct line *n = realloc(b->lines, (size_t)cap * sizeof(*n));
    if (!n)
      return;
    b->lines = n;
    b->cap = cap;
  }
  l = &b->lines[b->nlines++];
  l->n = b->ncur;
  l->c = b->ncur ? malloc((size_t)b->ncur * sizeof(struct cell)) : 0;
  if (l->c)
    memcpy(l->c, b->cur, (size_t)b->ncur * sizeof(struct cell));
  else
    l->n = 0;
  b->blank = b->ncur == 0;
  b->ncur = 0;
  b->pending_space = 0;
}

static void put_cell(struct browser *b, unsigned char ch)
{
  if (b->ncur == 0)
    while (b->ncur < b->indent && b->ncur < b->cols - 8)
    {
      b->cur[b->ncur].ch = ' ';
      b->cur[b->ncur].style = ST_TEXT;
      b->cur[b->ncur++].link = 0;
    }
  if (b->ncur >= b->cols || b->ncur >= MAX_COLS)
  {
    if (b->pre)
      return;                           /* preformatted: cut */
    /* wrap: move the last word to the next line */
    {
      struct cell word[MAX_COLS];
      int n = 0, i = b->ncur;
      while (i > b->indent && b->cur[i - 1].ch != ' ')
        i--;
      if (i > b->indent)
      {
        n = b->ncur - i;
        memcpy(word, b->cur + i, (size_t)n * sizeof(struct cell));
        b->ncur = i;
        while (b->ncur > 0 && b->cur[b->ncur - 1].ch == ' ')
          b->ncur--;
      }
      flush_line(b);
      while (b->ncur < b->indent && b->ncur < b->cols - 8)
      {
        b->cur[b->ncur].ch = ' ';
        b->cur[b->ncur].style = ST_TEXT;
        b->cur[b->ncur++].link = 0;
      }
      memcpy(b->cur + b->ncur, word, (size_t)n * sizeof(struct cell));
      b->ncur += n;
    }
  }
  b->cur[b->ncur].ch = ch;
  b->cur[b->ncur].style = (unsigned char)(b->link ? ST_LINK : b->pre ? ST_PRE : b->style);
  b->cur[b->ncur++].link = (unsigned short)b->link;
}

/* a character of the text (code page 437) */
static void put_text(struct browser *b, unsigned char ch)
{
  if (b->pre)
  {
    if (ch == '\n')
      flush_line(b);
    else if (ch == '\t')
      do
        put_cell(b, ' ');
      while (b->ncur % 8);
    else if (ch != '\r')
      put_cell(b, ch);
    return;
  }
  if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r')
  {
    b->pending_space = b->ncur > b->indent;
    return;
  }
  if (b->pending_space)
  {
    b->pending_space = 0;
    put_cell(b, ' ');
  }
  put_cell(b, ch);
}

static void put_string(struct browser *b, const char *s)
{
  for (; *s; s++)
    put_text(b, (unsigned char)*s);
}

static void line_break(struct browser *b)
{
  if (b->ncur > b->indent || b->pre)
    flush_line(b);
  b->pending_space = 0;
}

static void paragraph(struct browser *b)
{
  line_break(b);
  if (!b->blank)
  {
    b->ncur = 0;
    flush_line(b);
  }
}

/* ---------------------------------------------------------------- */
/* addresses                                                        */

/* href, seen from the page at base, as an absolute address */
static void resolve(const char *base, const char *href, char *out, size_t n)
{
  char root[512];
  const char *p;

  while (*href == ' ')
    href++;
  if (strstr(href, "://") || !strncasecmp(href, "about:", 6) || !strncasecmp(href, "mailto:", 7))
  {
    snprintf(out, n, "%s", href);
    return;
  }
  snprintf(root, sizeof(root), "%s", base);
  p = strstr(root, "://");
  p = p ? strchr(p + 3, '/') : 0;
  if (!strncmp(href, "//", 2))
    snprintf(out, n, "%s:%s", strncasecmp(base, "https:", 6) ? "http" : "https", href);
  else if (*href == '/')
  {
    if (p)
      root[p - root] = 0;
    snprintf(out, n, "%s%s", root, href);
  }
  else if (*href == '#')
    snprintf(out, n, "%s", base);
  else
  {
    char *q = strchr(root, '?'), *slash;
    if (q)
      *q = 0;
    slash = strrchr(root, '/');
    if (p && slash >= root + (p - root))
      slash[1] = 0;
    else
      strcat(root, "/");
    snprintf(out, n, "%s%s", root, href);
  }
  if ((p = strchr(out, '#')))
    out[p - out] = 0;
}

/* ---------------------------------------------------------------- */
/* HTML                                                             */

static const struct { const char *name; const char *text; } entities[] = {
  { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
  { "copy", "(c)" }, { "reg", "(R)" }, { "trade", "(TM)" }, { "mdash", "--" }, { "ndash", "-" },
  { "hellip", "..." }, { "laquo", "\xae" }, { "raquo", "\xaf" }, { "bull", "\x07" }, { "middot", "\xfa" },
  { "lsquo", "'" }, { "rsquo", "'" }, { "ldquo", "\"" }, { "rdquo", "\"" }, { "deg", "\xf8" },
  { "euro", "EUR" }, { "pound", "\x9c" }, { "times", "x" }, { "rarr", "->" }, { "larr", "<-" },
  { "uuml", "\x81" }, { "eacute", "\x82" }, { "auml", "\x84" }, { "agrave", "\x85" }, { "ccedil", "\x87" },
  { "egrave", "\x8a" }, { "Auml", "\x8e" }, { "Eacute", "\x90" }, { "ouml", "\x94" }, { "Ouml", "\x99" },
  { "Uuml", "\x9a" }, { "szlig", "\xe1" }, { "ntilde", "\xa4" }, { "Ntilde", "\xa5" }, { "aacute", "\xa0" },
  { "iacute", "\xa1" }, { "oacute", "\xa2" }, { "uacute", "\xa3" },
};

static void put_unicode(struct browser *b, unsigned long u)
{
  unsigned char c;

  if (u == 0xa0)
    c = ' ';
  else if (u == 0x2018 || u == 0x2019)
    c = '\'';
  else if (u == 0x201c || u == 0x201d)
    c = '"';
  else if (u == 0x2013 || u == 0x2014)
    c = '-';
  else if (u == 0x2026)
  {
    put_string(b, "...");
    return;
  }
  else
    c = cp437_from_unicode(u);
  if (u == '\n' || u == '\t' || u == '\r')
    c = (unsigned char)u;
  if (!c)
    c = '?';
  if (u == 0xa0 && !b->pre)
    put_cell(b, ' ');                   /* a space that stays */
  else
    put_text(b, c);
}

static int attr(const char *tag, const char *name, char *out, size_t n)
{
  const char *p = tag;
  size_t l = strlen(name), k = 0;

  out[0] = 0;
  while ((p = find_nocase(p, name)))
  {
    const char *v = p + l;
    if (p > tag && !isspace((unsigned char)p[-1]))
    {
      p += l;
      continue;
    }
    while (*v == ' ')
      v++;
    if (*v != '=')
    {
      p += l;
      continue;
    }
    v++;
    while (*v == ' ')
      v++;
    if (*v == '"' || *v == '\'')
    {
      char q = *v++;
      while (*v && *v != q && k + 1 < n)
        out[k++] = *v++;
    }
    else
      while (*v && !isspace((unsigned char)*v) && *v != '>' && k + 1 < n)
        out[k++] = *v++;
    out[k] = 0;
    return 1;
  }
  return 0;
}

static int tag_is(const char *tag, const char *name)
{
  size_t l = strlen(name);

  return !strncasecmp(tag, name, l) && (!tag[l] || isspace((unsigned char)tag[l]) || tag[l] == '/');
}

static void parse_html(struct browser *b, const char *h, size_t len)
{
  size_t i = 0;
  int list_depth = 0, ordered[8] = { 0 }, counter[8] = { 0 }, cell_no = 0, in_title = 0;
  size_t tl = 0;

  while (i < len)
  {
    unsigned char c = (unsigned char)h[i];
    if (c == '<')
    {
      char tag[600];
      const char *name;
      size_t j = i + 1, k = 0;
      int closing;

      if (!strncmp(h + i, "<!--", 4))
      {
        const char *e = strstr(h + i + 4, "-->");
        i = e ? (size_t)(e - h) + 3 : len;
        continue;
      }
      while (j < len && h[j] != '>' && k + 1 < sizeof(tag))
        tag[k++] = h[j++];
      tag[k] = 0;
      while (j < len && h[j] != '>')
        j++;
      i = j + 1;
      name = tag;
      closing = *name == '/';
      if (closing)
        name++;
      if (tag_is(name, "script") || tag_is(name, "style") || tag_is(name, "svg") || tag_is(name, "noscript"))
      {
        if (!closing)                   /* skip to its end tag */
        {
          char end[16];
          const char *e;
          snprintf(end, sizeof(end), "</%.8s", name);
          end[2 + strcspn(name, " />\t\r\n")] = 0;
          e = find_nocase(h + i, end);
          i = e ? (size_t)(e - h) : len;
        }
        continue;
      }
      if (tag_is(name, "title"))
      {
        in_title = !closing;
        continue;
      }
      if (tag_is(name, "br"))
        flush_line(b);
      else if (tag_is(name, "p") || tag_is(name, "div") || tag_is(name, "table") || tag_is(name, "form") ||
               tag_is(name, "blockquote") || tag_is(name, "section") || tag_is(name, "article") ||
               tag_is(name, "header") || tag_is(name, "footer") || tag_is(name, "nav") ||
               tag_is(name, "main") || tag_is(name, "center") || tag_is(name, "dl"))
        tag_is(name, "p") || tag_is(name, "table") || tag_is(name, "blockquote") ? paragraph(b) : line_break(b);
      else if (name[0] && (name[0] == 'h' || name[0] == 'H') && name[1] >= '1' && name[1] <= '6' &&
               (!name[2] || isspace((unsigned char)name[2])))
      {
        paragraph(b);
        b->style = closing ? ST_TEXT : ST_HEAD;
      }
      else if (tag_is(name, "hr"))
      {
        int n;
        line_break(b);
        for (n = 0; n < b->cols - b->indent; n++)
          put_cell(b, 0xc4);
        flush_line(b);
      }
      else if (tag_is(name, "ul") || tag_is(name, "ol"))
      {
        line_break(b);
        if (!closing && list_depth < 7)
        {
          list_depth++;
          ordered[list_depth] = tag_is(name, "ol");
          counter[list_depth] = 0;
        }
        else if (closing && list_depth > 0)
          list_depth--;
        b->indent = list_depth * 3;
        if (!list_depth)
          paragraph(b);
      }
      else if (tag_is(name, "li") && !closing)
      {
        char num[24];
        line_break(b);
        b->indent = list_depth > 1 ? (list_depth - 1) * 3 : 0;
        if (list_depth && ordered[list_depth])
          snprintf(num, sizeof(num), "%d. ", ++counter[list_depth]);
        else
          strcpy(num, "\x07 ");
        put_string(b, num);
        put_cell(b, ' ');
        b->ncur--;
        b->indent = (list_depth ? list_depth : 1) * 3;
      }
      else if (tag_is(name, "dt") || tag_is(name, "dd"))
      {
        line_break(b);
        b->indent = tag_is(name, "dd") && !closing ? 4 : 0;
      }
      else if (tag_is(name, "pre") || tag_is(name, "textarea"))
      {
        paragraph(b);
        b->pre = !closing;
      }
      else if (tag_is(name, "tr"))
      {
        line_break(b);
        cell_no = 0;
      }
      else if ((tag_is(name, "td") || tag_is(name, "th")) && !closing)
      {
        if (cell_no++)
          put_string(b, " \xb3 ");
        b->style = tag_is(name, "th") ? ST_BOLD : ST_TEXT;
      }
      else if (tag_is(name, "b") || tag_is(name, "strong") || tag_is(name, "em") || tag_is(name, "i"))
        b->style = closing ? ST_TEXT : ST_BOLD;
      else if (tag_is(name, "a"))
      {
        char href[400], abs[512];
        if (closing)
          b->link = 0;
        else if (attr(tag, "href", href, sizeof(href)) && b->nlinks < MAX_LINKS)
        {
          if (b->pending_space)         /* the space before a link is not part of it */
          {
            b->pending_space = 0;
            put_cell(b, ' ');
          }
          char **n = realloc(b->links, (size_t)(b->nlinks + 1) * sizeof(char *));
          resolve(b->url, href, abs, sizeof(abs));
          if (n)
          {
            b->links = n;
            if ((b->links[b->nlinks] = strdup(abs)))
              b->link = ++b->nlinks;
          }
        }
      }
      else if (tag_is(name, "img"))
      {
        char alt[80];
        put_text(b, '[');
        put_string(b, attr(tag, "alt", alt, sizeof(alt)) && alt[0] ? alt : "image");
        put_text(b, ']');
      }
      else if (tag_is(name, "input"))
      {
        char type[16], val[60];
        attr(tag, "type", type, sizeof(type));
        if (strcasecmp(type, "hidden"))
        {
          put_text(b, '[');
          put_string(b, attr(tag, "value", val, sizeof(val)) && val[0] ? val : "______");
          put_text(b, ']');
        }
      }
      continue;
    }
    /* text */
    if (in_title)
    {
      if (tl + 1 < sizeof(b->title) && c >= ' ')
        b->title[tl++] = (char)(c < 128 ? c : '?'), b->title[tl] = 0;
      i++;
      continue;
    }
    if (c == '&')
    {
      char name[12];
      size_t k = 0, j = i + 1;
      unsigned e;
      while (j < len && k + 1 < sizeof(name) && (isalnum((unsigned char)h[j]) || h[j] == '#'))
        name[k++] = h[j++];
      name[k] = 0;
      if (j < len && h[j] == ';' && k)
      {
        if (name[0] == '#')
        {
          put_unicode(b, strtoul(name + (tolower((unsigned char)name[1]) == 'x' ? 2 : 1), 0,
                                 tolower((unsigned char)name[1]) == 'x' ? 16 : 10));
          i = j + 1;
          continue;
        }
        for (e = 0; e < sizeof(entities) / sizeof(entities[0]); e++)
          if (!strcmp(name, entities[e].name))
          {
            if (!strcmp(name, "nbsp"))
              put_unicode(b, 0xa0);
            else
              put_string(b, entities[e].text);
            break;
          }
        if (e < sizeof(entities) / sizeof(entities[0]))
        {
          i = j + 1;
          continue;
        }
      }
      put_text(b, '&');
      i++;
      continue;
    }
    if (c >= 0xc0)                      /* UTF-8 */
    {
      int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1, k;
      unsigned long u = c & (0x3f >> n);
      if (i + (size_t)n < len)
      {
        for (k = 1; k <= n; k++)
          u = u << 6 | ((unsigned char)h[i + (size_t)k] & 0x3f);
        put_unicode(b, u);
        i += (size_t)n + 1;
        continue;
      }
    }
    put_text(b, c < 0x80 ? c : cp437_from_unicode(c) ? cp437_from_unicode(c) : '?');
    i++;
  }
  line_break(b);
}

static void parse_text(struct browser *b, const char *t, size_t len)
{
  size_t i;

  b->pre = 1;
  for (i = 0; i < len; i++)
    put_text(b, (unsigned char)t[i]);
  line_break(b);
  b->pre = 0;
}

/* ---------------------------------------------------------------- */
/* the window                                                       */

static void paint(struct widget *w, struct surface *s)
{
  struct browser *b = w->user;
  static const color_t ink[] = { RGB(0x20, 0x24, 0x2c), RGB(0x00, 0x00, 0x00), RGB(0x10, 0x30, 0x70),
                                 RGB(0x10, 0x50, 0xc0), RGB(0x30, 0x30, 0x30), RGB(0x70, 0x70, 0x78) };
  int r, c;

  draw_fill(s, w->x, w->y, w->w, w->h, RGB(0xff, 0xff, 0xff));
  draw_frame(s, w->x, w->y, w->w, w->h, RGB(0x90, 0x98, 0xa8));
  for (r = 0; r < b->rows && b->top + r < b->nlines; r++)
  {
    struct line *l = &b->lines[b->top + r];
    int y = w->y + 4 + r * FONT_H;
    for (c = 0; c < l->n && c < b->cols; c++)
    {
      char t[2] = { (char)l->c[c].ch, 0 };
      int x = w->x + 6 + c * FONT_W;
      if (l->c[c].ch == ' ' && l->c[c].style != ST_LINK)
        continue;
      draw_text(s, x, y, t, ink[l->c[c].style], l->c[c].style == ST_PRE ? RGB(0xf0, 0xf0, 0xf4) : TEXT_TRANSPARENT);
      if (l->c[c].style == ST_LINK)
        draw_hline(s, x, y + FONT_H - 2, FONT_W, ink[ST_LINK]);
      else if (l->c[c].style == ST_HEAD || l->c[c].style == ST_BOLD)
        draw_text(s, x + 1, y, t, ink[l->c[c].style], TEXT_TRANSPARENT);   /* bold */
    }
  }
}

static void show_status(struct browser *b, const char *text)
{
  ui_set_text(b->status, text);
  wm_update();
}

static void test_log(struct browser *b)
{
  int r, c, links = 0;

  tlog("MDDESK: browser loaded %s title=%s lines=%d links=%d\n", b->url, b->title, b->nlines, b->nlinks);
  for (r = 0; r < b->rows && r < b->nlines; r++)
    for (c = 0; c < b->lines[r].n; c++)
      if (b->lines[r].c[c].link && (c == 0 || b->lines[r].c[c - 1].link != b->lines[r].c[c].link) &&
          links++ < 8)
      {
        int sx, sy;
        ui_widget_center(b->view, &sx, &sy);
        tlog("MDDESK: browser link %d at %d,%d\n", b->lines[r].c[c].link,
             sx - b->view->w / 2 + 6 + c * FONT_W + 4, sy - b->view->h / 2 + 4 + r * FONT_H + 8);
      }
}

struct mem {
  char *d;
  size_t len, cap;
};

static int to_mem(void *ctx, const void *data, size_t n)
{
  struct mem *m = ctx;

  if (m->len + n > MAX_PAGE)
    n = MAX_PAGE - m->len;              /* the rest is cut */
  if (m->len + n + 1 > m->cap)
  {
    size_t cap = (m->len + n + 1) * 2;
    char *d = realloc(m->d, cap);
    if (!d)
      return 1;
    m->d = d;
    m->cap = cap;
  }
  memcpy(m->d + m->len, data, n);
  m->len += n;
  m->d[m->len] = 0;
  return 0;
}

static const char start_page[] =
  "<title>modernDOS Browser</title><h1>modernDOS Browser</h1>"
  "<p>Type an address above and press Enter. This browser shows pages as text: headings, "
  "lists, links, tables and preformatted text, from <b>http</b> and <b>https</b> sites. "
  "It runs no scripts, so sites that are built by scripts in the page stay empty.</p>"
  "<p>In a page: click a link; <b>Up</b>, <b>Down</b>, <b>PgUp</b>, <b>PgDn</b>, <b>Home</b>, "
  "<b>End</b> scroll; <b>Backspace</b> goes back.</p>"
  "<h2>Places that read well as text</h2><ul>"
  "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a> - search</li>"
  "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search, pages made simple</li>"
  "<li><a href=\"https://en.wikipedia.org/\">Wikipedia</a></li>"
  "<li><a href=\"https://text.npr.org/\">NPR text news</a></li>"
  "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a></li>"
  "<li><a href=\"https://www.freedos.org/\">FreeDOS</a></li></ul>";

static void go(struct browser *b, const char *url, int remember)
{
  struct mem m = { 0, 0, 0 };
  char err[200], type[64], full[512], msg[900];
  int ok = 1;

  if (!strstr(url, "://") && strncasecmp(url, "about:", 6))
    snprintf(full, sizeof(full), "http://%s", url);
  else
    snprintf(full, sizeof(full), "%s", url);
  if (remember && b->url[0] && b->nhist < HISTORY)
    strcpy(b->history[b->nhist++], b->url);
  else if (remember && b->url[0])
  {
    memmove(b->history[0], b->history[1], sizeof(b->history[0]) * (HISTORY - 1));
    strcpy(b->history[HISTORY - 1], b->url);
  }
  page_clear(b);
  snprintf(b->url, sizeof(b->url), "%s", full);
  ui_set_text(b->addr, full);
  type[0] = 0;
  if (!strncasecmp(full, "about:", 6))
    parse_html(b, start_page, sizeof(start_page) - 1);
  else
  {
    snprintf(msg, sizeof(msg), "Loading %.200s ...", full);
    show_status(b, msg);
    if (!net_up && !(net_up = http_init()))
    {
      snprintf(err, sizeof(err), "No network: load the packet driver (PKTDRV) before the desktop.");
      ok = 0;
    }
    else if (http_get(full, to_mem, &m, type, err, sizeof(err)))
      ok = 0;
    wm_redraw_all();                    /* the network code may have written text */
    if (!ok)
    {
      snprintf(msg, sizeof(msg), "<title>Not loaded</title><h1>The page could not be loaded</h1><p>%s</p>"
               "<p>%.300s</p>", err, full);
      parse_html(b, msg, strlen(msg));
    }
    else if (!m.d)
      parse_html(b, "<p>(an empty page)</p>", 22);
    else if (!strncasecmp(type, "text/plain", 10))
      parse_text(b, m.d, m.len);
    else if (!type[0] || !strncasecmp(type, "text/", 5) || find_nocase(type, "html") || find_nocase(type, "xml"))
      parse_html(b, m.d, m.len);
    else
    {
      snprintf(msg, sizeof(msg), "<h1>Cannot show this</h1><p>The address holds a file of the kind "
               "<b>%.60s</b> (%lu bytes), not a page.</p>", type, (unsigned long)m.len);
      parse_html(b, msg, strlen(msg));
    }
    free(m.d);
  }
  snprintf(msg, sizeof(msg), "%.60s%s%d lines, %d links", b->title, b->title[0] ? " - " : "", b->nlines,
           b->nlinks);
  ui_set_text(b->status, msg);
  ui_redraw(b->ui);
  test_log(b);
}

static void scroll_to(struct browser *b, int top)
{
  if (top > b->nlines - b->rows)
    top = b->nlines - b->rows;
  if (top < 0)
    top = 0;
  b->top = top;
}

static void back(struct browser *b)
{
  char url[512];

  if (!b->nhist)
    return;
  strcpy(url, b->history[--b->nhist]);
  go(b, url, 0);
}

static int input(struct widget *w, const struct event *e)
{
  struct browser *b = w->user;

  if (e->type == EV_MOUSE_DOWN)
  {
    int r = (e->y - w->y - 4) / FONT_H + b->top, c = (e->x - w->x - 6) / FONT_W;
    if (r >= 0 && r < b->nlines && c >= 0 && c < b->lines[r].n && b->lines[r].c[c].link)
    {
      char url[512];
      snprintf(url, sizeof(url), "%s", b->links[b->lines[r].c[c].link - 1]);
      go(b, url, 1);
    }
    return 1;
  }
  if (e->type != EV_KEY)
    return 0;
  switch (e->scan)
  {
    case 0x48: scroll_to(b, b->top - 1); return 1;
    case 0x50: scroll_to(b, b->top + 1); return 1;
    case 0x49: scroll_to(b, b->top - (b->rows - 1)); return 1;
    case 0x51: scroll_to(b, b->top + (b->rows - 1)); return 1;
    case 0x47: scroll_to(b, 0); return 1;
    case 0x4f: scroll_to(b, b->nlines); return 1;
    case 0x0e: back(b); return 1;
  }
  if (e->ascii == ' ')
  {
    scroll_to(b, b->top + (b->rows - 1));
    return 1;
  }
  return 0;
}

static void a_go(struct widget *w)
{
  struct browser *b = w->user;
  char url[130];

  snprintf(url, sizeof(url), "%s", b->addr->text);
  go(b, url, 1);
  ui_focus(b->view);
}

static void a_back(struct widget *w)
{
  back(w->user);
}

static int events(struct window *w, const struct event *e)
{
  struct browser *b = the;

  if (e->type != EV_CLOSE || !b || b->win != w)
    return 0;
  page_clear(b);
  free(b->lines);
  free(b->links);
  wm_window_close(w);
  free(b);
  the = 0;
  panel_damage();
  tlog("MDDESK: browser closed\n");
  return 1;
}

void browser_open(const char *url)
{
  struct browser *b;
  struct widget *go_button, *back_button;
  int W = 900, H = 600;

  if (the)
  {
    wm_raise(the->win);
    if (url)
      go(the, url, 1);
    return;
  }
  if (!(b = calloc(1, sizeof(*b))) || !(b->win = wm_window_new(50, 44, W, H, "Browser")))
  {
    free(b);
    return;
  }
  the = b;
  b->win->handler = events;
  b->ui = ui_new(b->win);
  W = b->win->client->w;
  H = b->win->client->h;
  back_button = ui_button(b->ui, 8, 8, 60, 26, "Back", a_back);
  back_button->user = b;
  b->addr = ui_textfield(b->ui, 76, 9, W - 76 - 68, a_go);
  b->addr->user = b;
  go_button = ui_button(b->ui, W - 60, 8, 52, 26, "Go", a_go);
  go_button->user = b;
  b->view = ui_canvas(b->ui, 8, 42, W - 16, H - 42 - 28, paint, input);
  b->view->user = b;
  b->status = ui_label(b->ui, 10, H - 22, "");
  b->cols = (b->view->w - 12) / FONT_W;
  if (b->cols > MAX_COLS)
    b->cols = MAX_COLS;
  b->rows = (b->view->h - 8) / FONT_H;
  desk_widget("browser_addr", b->addr);
  desk_widget("browser_view", b->view);
  tlog("MDDESK: window Browser at %d,%d\n", b->win->x, b->win->y);
  go(b, url ? url : "about:start", 0);
  ui_focus(b->addr);
  panel_damage();
}
