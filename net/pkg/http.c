/* a small HTTP/1.0 client: GET with redirects, body until the server closes */
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

/* "http://host[:port]/path" -> host, port, path; 0 if it is not that */
static int split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
  const char *p, *slash, *colon;
  size_t n;

  if (strncasecmp(url, "http://", 7))
    return 0;
  p = url + 7;
  slash = strchr(p, '/');
  if (!slash)
    slash = p + strlen(p);
  colon = memchr(p, ':', (size_t)(slash - p));
  n = (size_t)((colon ? colon : slash) - p);
  if (!n || n >= hn)
    return 0;
  memcpy(host, p, n);
  host[n] = 0;
  *port = colon ? atoi(colon + 1) : 80;
  snprintf(path, pn, "%s", *slash ? slash : "/");
  return *port > 0 && *port < 65536;
}

static int get_once(const char *url, http_sink sink, void *ctx, char *type, char *next, size_t nextn,
                    char *err, size_t errn)
{
  char host[128], path[512], req[800], buf[2048], head[4096];
  struct sockaddr_in sa;
  struct hostent *h;
  size_t hlen = 0;
  int port, s, n, status = 0, body = 0;
  char *p, *line;

  next[0] = 0;
  if (!strncasecmp(url, "https://", 8))
    return fail(err, errn, "%s: https is not supported yet", url);
  if (!split(url, host, sizeof(host), &port, path, sizeof(path)))
    return fail(err, errn, "%s: not an http:// address", url);
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
  snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: modernDOS/0.1\r\n"
           "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
  if (send(s, req, (int)strlen(req), 0) < 0)
  {
    CLOSE(s);
    return fail(err, errn, "%s: cannot send the request", host);
  }
  while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
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
      if (!p && hlen < sizeof(head) - 1)
        continue;
      if (!p)
      {
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
            snprintf(next, nextn, "http://%s:%d%s", host, port, v);
          else
            snprintf(next, nextn, "%s", v);
        }
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
        CLOSE(s);
        snprintf(code, sizeof(code), "%d", status);
        return fail(err, errn, status ? "the server answered %s" : "no HTTP answer%s", status ? code : "");
      }
      /* body bytes that came with the header */
      data = p + 4;
      n = (int)(hlen - (size_t)(data - head));
      if (n <= 0)
        continue;
    }
    if (sink(ctx, data, (size_t)n))
    {
      CLOSE(s);
      return fail(err, errn, "%s", "stopped (cannot store the data?)");
    }
  }
  CLOSE(s);
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
