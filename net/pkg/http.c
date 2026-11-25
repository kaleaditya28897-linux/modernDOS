/* a small HTTP/1.1 client: GET with redirects; the body by its length, in
   chunks, or until the server closes;
   http:// and https:// (TLS: tls.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef __DJGPP__
#include <tcp.h>
#define CLOSE closesocket
#else
#define CLOSE close
#endif
#include "http.h"
#include "tls.h"

int http_init(void)
{
#ifdef __DJGPP__
  _watt_do_exit = 0;                    /* a failure is ours to report */
  return sock_init() == 0;
#else
  return 1;
#endif
}

static int fail(char *err, size_t errn, const char *fmt, const char *arg)
{
  snprintf(err, errn, fmt, arg);
  return -1;
}

/* "http[s]://host[:port]/path" -> host, port, path, secure; 0 if it is
   not that */
static int split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn, int *secure)
{
  const char *p, *slash, *colon;
  size_t n;

  *secure = !strncasecmp(url, "https://", 8);
  if (!*secure && strncasecmp(url, "http://", 7))
    return 0;
  p = url + (*secure ? 8 : 7);
  slash = strchr(p, '/');
  if (!slash)
    slash = p + strlen(p);
  colon = memchr(p, ':', (size_t)(slash - p));
  n = (size_t)((colon ? colon : slash) - p);
  if (!n || n >= hn)
    return 0;
  memcpy(host, p, n);
  host[n] = 0;
  *port = colon ? atoi(colon + 1) : *secure ? 443 : 80;
  snprintf(path, pn, "%s", *slash ? slash : "/");
  return *port > 0 && *port < 65536;
}

/* the body's framing: a length, chunks, or all until the close */
struct body {
  int chunked, done;
  long left;                            /* bytes of the body / the chunk; -1: unknown */
  int state;                            /* chunked: 0 size line, 1 data, 2 CRLF after data */
  char num[20];
  int nnum;
};

/* body bytes as received -> the sink; 1 if the sink refused */
static int body_data(struct body *b, const char *d, size_t n, http_sink sink, void *ctx)
{
  while (n && !b->done)
  {
    size_t k;
    if (!b->chunked)
    {
      k = b->left >= 0 && (size_t)b->left < n ? (size_t)b->left : n;
      if (k && sink(ctx, d, k))
        return 1;
      if (b->left >= 0 && !(b->left -= (long)k))
        b->done = 1;
      return 0;
    }
    if (b->state == 0)                  /* "hex[;ext]\r\n" */
    {
      char c = *d++;
      n--;
      if (c == '\n')
      {
        b->num[b->nnum] = 0;
        b->nnum = 0;
        b->left = strtol(b->num, 0, 16);
        if (b->left <= 0)
          b->done = 1;                  /* the last chunk (trailers are not read) */
        else
          b->state = 1;
      }
      else if (b->nnum < (int)sizeof(b->num) - 1 && c != '\r')
        b->num[b->nnum++] = c;
    }
    else if (b->state == 1)
    {
      k = (size_t)b->left < n ? (size_t)b->left : n;
      if (sink(ctx, d, k))
        return 1;
      d += k;
      n -= k;
      if (!(b->left -= (long)k))
        b->state = 2;
    }
    else                                /* the CRLF after a chunk */
    {
      if (*d == '\n')
        b->state = 0;
      d++;
      n--;
    }
  }
  return 0;
}

static int get_once(const char *url, http_sink sink, void *ctx, char *type, char *next, size_t nextn,
                    char *err, size_t errn)
{
  char host[128], path[512], req[800], buf[2048];
  static char head[16384];              /* some sites send 8 KB of header */
  struct sockaddr_in sa;
  struct hostent *h;
  size_t hlen = 0;
  int port, s, n = 0, status = 0, body = 0, secure;
  struct tls *t = 0;
  struct body bd;
  char *p, *line;

  next[0] = 0;
  memset(&bd, 0, sizeof(bd));
  bd.left = -1;
  if (!split(url, host, sizeof(host), &port, path, sizeof(path), &secure))
    return fail(err, errn, "%s: not an http:// or https:// address", url);
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  sa.sin_addr.s_addr = inet_addr(host);
  if (sa.sin_addr.s_addr == INADDR_NONE)
  {
    if (!(h = gethostbyname(host)))
      return fail(err, errn, "%s: unknown host", host);
    memcpy(&sa.sin_addr, h->h_addr, sizeof(sa.sin_addr));
  }
  if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0 || connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0)
  {
    if (s >= 0)
      CLOSE(s);
    return fail(err, errn, "%s: cannot connect", host);
  }
  snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: modernDOS/0.1\r\n"
           "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
  if (secure && !(t = tls_open(s, host, err, errn)))
  {
    CLOSE(s);
    return -1;
  }
  if (t ? tls_write(t, req, strlen(req)) : send(s, req, (int)strlen(req), 0) < 0)
  {
    tls_close(t);
    CLOSE(s);
    return fail(err, errn, "%s: cannot send the request", host);
  }
  while (!bd.done && (n = t ? tls_read(t, buf, sizeof(buf)) : recv(s, buf, sizeof(buf), 0)) > 0)
  {
    const char *data = buf;
    if (!body)
    {
      /* collect the header (to the empty line) */
      size_t k = (size_t)n < sizeof(head) - 1 - hlen ? (size_t)n : sizeof(head) - 1 - hlen;
      memcpy(head + hlen, buf, k);
      hlen += k;
      head[hlen] = 0;
      p = strstr(head, "\r\n\r\n");
      if (!p && k == (size_t)n)
        continue;
      if (!p)
      {
        tls_close(t);
        CLOSE(s);
        return fail(err, errn, "%s: the answer's header is too long", host);
      }
      *p = 0;
      body = 1;
      if (sscanf(head, "HTTP/%*s %d", &status) != 1)
        status = 0;
      for (line = strtok(head, "\r\n"); line; line = strtok(NULL, "\r\n"))
      {
        if (!strncasecmp(line, "Location:", 9))
        {
          const char *v = line + 9;
          while (*v == ' ')
            v++;
          if (*v == '/')
            snprintf(next, nextn, "%s://%s:%d%s", secure ? "https" : "http", host, port, v);
          else
            snprintf(next, nextn, "%s", v);
        }
        if (!strncasecmp(line, "Content-Length:", 15))
          bd.left = atol(line + 15);
        if (!strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line + 18, "chunked"))
          bd.chunked = 1;
        if (type && !strncasecmp(line, "Content-Type:", 13))
        {
          const char *v = line + 13;
          while (*v == ' ')
            v++;
          snprintf(type, 64, "%s", v);
        }
      }
      if (status >= 300 && status < 400 && next[0])
        break;                          /* the caller follows */
      next[0] = 0;
      if (status != 200)
      {
        char code[16];
        tls_close(t);
        CLOSE(s);
        snprintf(code, sizeof(code), "%d", status);
        return fail(err, errn, status ? "the server answered %s" : "no HTTP answer%s", status ? code : "");
      }
      /* body bytes that came with the header */
      data = p + 4;
      n = (int)(hlen - (size_t)(data - head));
      if (bd.left == 0 && !bd.chunked)
        bd.done = 1;
      if (n <= 0)
        continue;
    }
    if (body_data(&bd, data, (size_t)n, sink, ctx))
    {
      tls_close(t);
      CLOSE(s);
      return fail(err, errn, "%s", "stopped (cannot store the data?)");
    }
  }
  tls_close(t);
  CLOSE(s);
  if (n < 0 && t)
    return fail(err, errn, "%s: the secure connection broke", host);
  if (!body)
    return fail(err, errn, "%s: no answer", host);
  return 0;
}

int http_get(const char *url, http_sink sink, void *ctx, char *type, char *err, size_t errn)
{
  char cur[600], next[600];
  int i;

  snprintf(cur, sizeof(cur), "%s", url);
  if (type)
    type[0] = 0;
  for (i = 0; i < 6; i++)
  {
    if (get_once(cur, sink, ctx, type, next, sizeof(next), err, errn))
      return -1;
    if (!next[0])
      return 0;
    snprintf(cur, sizeof(cur), "%s", next);
  }
  return fail(err, errn, "%s", "too many redirects");
}
