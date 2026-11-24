/* SSH transport layer (RFC 4253) for the modernDOS SSH server and client
 *
 * One fixed algorithm set, chosen for small code and good security:
 *   key exchange  curve25519-sha256 (RFC 8731), with OpenSSH's "strict KEX"
 *                 (kex-strict-*-v00@openssh.com, against the Terrapin attack)
 *   host key      ssh-ed25519
 *   cipher        chacha20-poly1305@openssh.com (no separate MAC)
 *   compression   none
 */
#ifndef SSH_H
#define SSH_H
#include <stdint.h>
#include "buf.h"

enum {
  SSH_MSG_DISCONNECT = 1, SSH_MSG_IGNORE = 2, SSH_MSG_UNIMPLEMENTED = 3,
  SSH_MSG_DEBUG = 4, SSH_MSG_SERVICE_REQUEST = 5, SSH_MSG_SERVICE_ACCEPT = 6,
  SSH_MSG_KEXINIT = 20, SSH_MSG_NEWKEYS = 21,
  SSH_MSG_KEX_ECDH_INIT = 30, SSH_MSG_KEX_ECDH_REPLY = 31,
  SSH_MSG_USERAUTH_REQUEST = 50, SSH_MSG_USERAUTH_FAILURE = 51,
  SSH_MSG_USERAUTH_SUCCESS = 52, SSH_MSG_USERAUTH_BANNER = 53,
  SSH_MSG_USERAUTH_PK_OK = 60,
  SSH_MSG_GLOBAL_REQUEST = 80, SSH_MSG_REQUEST_SUCCESS = 81, SSH_MSG_REQUEST_FAILURE = 82,
  SSH_MSG_CHANNEL_OPEN = 90, SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91,
  SSH_MSG_CHANNEL_OPEN_FAILURE = 92, SSH_MSG_CHANNEL_WINDOW_ADJUST = 93,
  SSH_MSG_CHANNEL_DATA = 94, SSH_MSG_CHANNEL_EXTENDED_DATA = 95,
  SSH_MSG_CHANNEL_EOF = 96, SSH_MSG_CHANNEL_CLOSE = 97,
  SSH_MSG_CHANNEL_REQUEST = 98, SSH_MSG_CHANNEL_SUCCESS = 99, SSH_MSG_CHANNEL_FAILURE = 100
};

enum {
  SSH_DISCONNECT_PROTOCOL_ERROR = 2, SSH_DISCONNECT_KEY_EXCHANGE_FAILED = 3,
  SSH_DISCONNECT_MAC_ERROR = 5, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE = 7,
  SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE = 9, SSH_DISCONNECT_BY_APPLICATION = 11,
  SSH_DISCONNECT_TOO_MANY_CONNECTIONS = 12, SSH_DISCONNECT_AUTH_CANCELLED = 13,
  SSH_DISCONNECT_NO_MORE_AUTH_METHODS = 14
};

#define SSH_MAX_PACKET 35000            /* RFC 4253: at least 35000 */

struct ssh_keys {
  uint8_t main[32];                     /* payload (K_2) */
  uint8_t hdr[32];                      /* length field (K_1) */
};

struct ssh {
  int fd;
  int server;
  char vc[256], vs[256];                /* identification strings, no CR LF */
  int version_done;
  struct buf in, out;                   /* raw bytes from / to the peer */
  struct buf deferred;                  /* payloads held back during a re-kex */
  struct buf i_c, i_s;                  /* the two KEXINIT payloads */
  uint32_t seq_in, seq_out;
  int rx_on, tx_on;
  struct ssh_keys rx, tx, nrx, ntx;     /* current and next keys */
  uint8_t sid[32];                      /* session identifier */
  int have_sid;
  int sent_kexinit, got_kexinit, sent_newkeys;
  int kex_done;                         /* the first key exchange finished */
  int strict;                           /* strict KEX negotiated */
  int ignore_next;                      /* wrong first_kex_packet_follows guess */
  uint8_t x_sk[32], x_pk[32];           /* our ephemeral X25519 key */
  const uint8_t *host_sk;               /* server: ed25519 secret key (64) */
  const uint8_t *host_pk;               /* server: ed25519 public key (32) */
  /* client: accept the server's host key? (ed25519 public key) */
  int (*check_hostkey)(struct ssh *s, const uint8_t pk[32]);
  uint8_t peer_hostkey[32];
  void *user;                           /* for the caller */
  int dead;                             /* connection unusable */
  char why[160];                        /* reason, for the log */
};

/* start a connection on a connected, non-blocking socket */
void ssh_start(struct ssh *s, int fd, int server);
void ssh_free(struct ssh *s);
/* move bytes between the socket and the buffers; 0 if nothing moved */
int ssh_io(struct ssh *s);
/* next payload for the caller (kex and transport messages are handled
   here): 1 = `payload` holds one (type first), 0 = none yet, -1 = dead */
int ssh_packet(struct ssh *s, struct buf *payload);
/* send a payload (first byte = message type); consumes it */
void ssh_send(struct ssh *s, struct buf *payload);
void ssh_disconnect(struct ssh *s, uint32_t code, const char *msg);
/* keys are up and no key exchange runs: channel traffic may be sent */
int ssh_ready(const struct ssh *s);
/* bytes waiting to go out (for flow control) */
size_t ssh_pending(const struct ssh *s);

/* ed25519 public key blob and its OpenSSH fingerprint "SHA256:..." */
void ssh_ed25519_blob(struct buf *b, const uint8_t pk[32]);
void ssh_fingerprint(const uint8_t pk[32], char out[64]);

/* platform: non-blocking socket I/O (net_*.c) */
int net_recv(int fd, void *p, int n);   /* >0 bytes, 0 nothing yet, -1 closed */
int net_send(int fd, const void *p, int n);

#endif
