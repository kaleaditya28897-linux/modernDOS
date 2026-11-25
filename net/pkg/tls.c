/*
 * TLS client connections with BearSSL: TLS 1.2 (and older, down to 1.0),
 * server certificates checked against the certificate authorities built
 * in at compile time (trust_anchors.h, Mozilla's list) and against the
 * host name, with the machine's clock for the validity dates.
 *
 * Random numbers on DOS: the SSH programs' generator (timing jitter, seed
 * file). On a build host BearSSL takes the system's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <bearssl.h>
#include "tls.h"
#include "trust_anchors.h"
#ifdef __DJGPP__
#include "../ssh/rng.h"
#endif

struct tls {
  br_ssl_client_context sc;
  br_x509_minimal_context xc;
  br_sslio_context io;
  unsigned char buf[BR_SSL_BUFSIZE_BIDI];
  int fd;
};

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
  int n = recv(*(int *)ctx, (char *)buf, (int)len, 0);

  return n > 0 ? n : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
  int n = send(*(int *)ctx, (const char *)buf, (int)len, 0);

  return n > 0 ? n : -1;
}

static const char *why(int e)
{
  switch (e)
  {
    case BR_ERR_X509_EXPIRED:
      return "the server's certificate is expired or not yet valid (is this machine's clock right?)";
    case BR_ERR_X509_NOT_TRUSTED:
      return "the server's certificate is not from a known authority";
    case BR_ERR_X509_BAD_SERVER_NAME:
      return "the server's certificate is for another name";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:
      return "the server needs a TLS version this program does not have (TLS 1.3 only?)";
    case BR_ERR_NO_RANDOM:
      return "no random numbers";
    case BR_ERR_IO:
      return "the connection was closed";
  }
  if (e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_RECV_FATAL_ALERT + 256)
    return "the server refused the TLS handshake";
  return 0;
}

static void explain(struct tls *t, char *err, size_t errn)
{
  int e = br_ssl_engine_last_error(&t->sc.eng);

  if (why(e))
    snprintf(err, errn, "secure connection failed: %s", why(e));
  else
    snprintf(err, errn, "secure connection failed (TLS error %d)", e);
}

struct tls *tls_open(int fd, const char *host, char *err, size_t errn)
{
  struct tls *t = malloc(sizeof(*t));
  time_t now = time(0);

  if (!t)
  {
    snprintf(err, errn, "no memory for a secure connection");
    return 0;
  }
  t->fd = fd;
  br_ssl_client_init_full(&t->sc, &t->xc, TAs, TAs_NUM);
  /* the validity dates: days since 0 AD (1970-01-01 is day 719528) */
  br_x509_minimal_set_time(&t->xc, (uint32_t)(now / 86400) + 719528, (uint32_t)(now % 86400));
#ifdef __DJGPP__
  {
    static int seeded;
    unsigned char seed[32];
    if (!seeded)
    {
      rng_init(0);
      seeded = 1;
    }
    rng_event((uint32_t)fd);
    rng_bytes(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof(seed));
  }
#endif
  br_ssl_engine_set_buffer(&t->sc.eng, t->buf, sizeof(t->buf), 1);
  if (!br_ssl_client_reset(&t->sc, host, 0))
  {
    explain(t, err, errn);
    free(t);
    return 0;
  }
  br_sslio_init(&t->io, &t->sc.eng, sock_read, &t->fd, sock_write, &t->fd);
  if (br_sslio_flush(&t->io) < 0)       /* runs the handshake */
  {
    explain(t, err, errn);
    free(t);
    return 0;
  }
  return t;
}

int tls_write(struct tls *t, const void *data, size_t n)
{
  return br_sslio_write_all(&t->io, data, n) < 0 || br_sslio_flush(&t->io) < 0 ? -1 : 0;
}

int tls_read(struct tls *t, void *buf, size_t n)
{
  int r = br_sslio_read(&t->io, buf, n);

  if (r >= 0)
    return r;
  /* a closed connection ends the data (HTTP/1.0 bodies end that way, and
     many servers do not send TLS's own end mark) */
  return br_ssl_engine_last_error(&t->sc.eng) == BR_ERR_OK ||
         br_ssl_engine_last_error(&t->sc.eng) == BR_ERR_IO ? 0 : -1;
}

void tls_close(struct tls *t)
{
  if (t)
  {
    br_sslio_close(&t->io);
    free(t);
  }
}
