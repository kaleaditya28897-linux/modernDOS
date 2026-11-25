/* HTTPGET url [file]: fetch a page (http or https) - a test of the HTTP
   client; prints the content type and size, or the error */
#include <stdio.h>
#include "http.h"

static unsigned long total;

static int sink(void *ctx, const void *data, size_t n)
{
  total += n;
  return ctx && fwrite(data, 1, n, ctx) != n;
}

int main(int argc, char **argv)
{
  char err[200], type[64];
  FILE *f = argc > 2 ? fopen(argv[2], "wb") : 0;

  if (argc < 2 || !http_init())
  {
    printf("HTTPGET: %s\n", argc < 2 ? "usage: HTTPGET url [file]" : "no network");
    return 2;
  }
  if (http_get(argv[1], sink, f, type, err, sizeof(err)))
  {
    printf("HTTPGET: FAILED %s\n", err);
    return 1;
  }
  if (f)
    fclose(f);
  printf("HTTPGET: ok %lu bytes, type %s\n", total, type);
  return 0;
}
