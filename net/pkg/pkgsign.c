/* pkgsign - repository keys and signatures for PKG (runs on the build
 * host, used by scripts/mkrepo.sh)
 *
 *   pkgsign keygen keyfile     new key: the secret (hex) into keyfile,
 *                              the public key (for PKG.CFG) to stdout
 *   pkgsign pub keyfile        the public key
 *   pkgsign sign keyfile file  the signature of file (hex) to stdout
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ssh/crypto/monocypher-ed25519.h"

static void puthex(const uint8_t *p, int n)
{
  while (n--)
    printf("%02x", *p++);
  printf("\n");
}

static void load(const char *file, uint8_t sk[64], uint8_t pk[32])
{
  uint8_t seed[32];
  char hex[80];
  unsigned v;
  int i;
  FILE *f = fopen(file, "r");

  if (!f || !fgets(hex, sizeof(hex), f) || strlen(hex) < 64)
  {
    fprintf(stderr, "pkgsign: cannot read the key %s\n", file);
    exit(1);
  }
  fclose(f);
  for (i = 0; i < 32; i++)
  {
    sscanf(hex + i * 2, "%2x", &v);
    seed[i] = (uint8_t)v;
  }
  crypto_ed25519_key_pair(sk, pk, seed);
}

int main(int argc, char **argv)
{
  uint8_t sk[64], pk[32], sig[64], seed[32];
  int i;

  if (argc == 3 && !strcmp(argv[1], "keygen"))
  {
    FILE *r = fopen("/dev/urandom", "rb"), *f;
    if (!r || fread(seed, 1, 32, r) != 32 || !(f = fopen(argv[2], "w")))
      return 1;
    fclose(r);
    for (i = 0; i < 32; i++)
      fprintf(f, "%02x", seed[i]);
    fprintf(f, "\n");
    fclose(f);
    crypto_ed25519_key_pair(sk, pk, seed);
    puthex(pk, 32);
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "pub"))
  {
    load(argv[2], sk, pk);
    puthex(pk, 32);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "sign"))
  {
    FILE *f = fopen(argv[3], "rb");
    static uint8_t data[1 << 20];
    size_t n;
    if (!f)
      return 1;
    n = fread(data, 1, sizeof(data), f);
    fclose(f);
    load(argv[2], sk, pk);
    crypto_ed25519_sign(sig, sk, data, n);
    puthex(sig, 64);
    return 0;
  }
  fprintf(stderr, "usage: pkgsign keygen keyfile | pub keyfile | sign keyfile file\n");
  return 2;
}
