/* a small HTTP/1.0 client (GET), for the package manager and the browser */
#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

/* data sink: return 0 to go on */
typedef int (*http_sink)(void *ctx, const void *data, size_t n);

/* GET url ("http://host[:port]/path"), follows redirects; 0 on success,
   else -1 with a message in err. *type (if not NULL) gets the content
   type (64 bytes). */
int http_get(const char *url, http_sink sink, void *ctx, char *type, char *err, size_t errn);

/* network up (Watt-32 on DOS); 0 on failure */
int http_init(void);

#endif
