/* TLS 1.2 client connections (BearSSL) for the HTTP client */
#ifndef TLS_H
#define TLS_H
#include <stddef.h>

struct tls;

/* TLS over the connected socket fd, to the server named host (its
   certificate is checked against the built-in authorities and the name).
   0 on failure, with a message in err. */
struct tls *tls_open(int fd, const char *host, char *err, size_t errn);
int tls_write(struct tls *t, const void *data, size_t n);  /* 0 ok, -1 */
int tls_read(struct tls *t, void *buf, size_t n);          /* bytes, 0 end, -1 */
void tls_close(struct tls *t);                             /* not the socket */

#endif
