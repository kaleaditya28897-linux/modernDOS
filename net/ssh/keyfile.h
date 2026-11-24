#ifndef KEYFILE_H
#define KEYFILE_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"

/* an unencrypted OpenSSH ed25519 private key; 0 + message on failure */
int key_read(const char *path, uint8_t sk[64], uint8_t pk[32], char *err, size_t errn);
int key_write(const char *path, const uint8_t sk[64], const uint8_t pk[32], const char *comment);
/* "ssh-ed25519 AAAA... comment" for authorized_keys */
void key_public_line(const uint8_t pk[32], const char *comment, char *out, size_t n);

#endif
