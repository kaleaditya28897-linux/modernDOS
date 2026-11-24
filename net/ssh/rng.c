/* random numbers for SSH
 *
 * DOS has no entropy source and the target CPUs (Core 2) have no RDRAND,
 * so: timing jitter between the CPU's time stamp counter and the 8254
 * timer (sampled many times at start), the time of day, a seed file kept
 * between runs, and the timing of later events (packets, keys) are hashed
 * into a BLAKE2b pool. Output: ChaCha20 keyed from the pool, with a new
 * key after every request (old output can not be recomputed).
 * On a host build (tests) /dev/urandom is used as well.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "crypto/monocypher.h"
#include "rng.h"
#ifdef __DJGPP__
#include <pc.h>
#endif

static crypto_blake2b_ctx pool;
static uint8_t key[32];
static uint64_t nonce;
static int ready;

static uint64_t tsc(void)
{
#if defined(__i386__) || defined(__x86_64__)
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)hi << 32 | lo;
#else
  return (uint64_t)clock();
#endif
}

void rng_event(uint32_t what)
{
  uint64_t t = tsc();

  if (!ready)
    return;
  crypto_blake2b_update(&pool, (const uint8_t *)&t, sizeof(t));
  crypto_blake2b_update(&pool, (const uint8_t *)&what, sizeof(what));
}

/* fold the pool into the output key */
static void reseed(void)
{
  uint8_t h[64];

  crypto_blake2b_update(&pool, key, sizeof(key));
  crypto_blake2b_final(&pool, h);
  memcpy(key, h, 32);
  crypto_blake2b_init(&pool, 64);
  crypto_blake2b_update(&pool, h + 32, 32);     /* carry the rest along */
  crypto_wipe(h, sizeof(h));
}

static void jitter(int samples)
{
  int i;

  for (i = 0; i < samples; i++)
  {
    uint64_t t0 = tsc(), t1;
#ifdef __DJGPP__
    uint8_t a, b;
    int spin = 0;
    outportb(0x43, 0x00);               /* latch counter 0 */
    a = inportb(0x40);
    (void)inportb(0x40);
    do
    {
      outportb(0x43, 0x00);
      b = inportb(0x40);
      (void)inportb(0x40);
    } while (b == a && ++spin < 100000);  /* no 8254: TSC alone */
#endif
    t1 = tsc();
    t1 -= t0;
    crypto_blake2b_update(&pool, (const uint8_t *)&t1, sizeof(t1));
  }
}

void rng_init(const char *seedfile)
{
  uint8_t buf[64];
  time_t now = time(NULL);
  FILE *f;

  crypto_blake2b_init(&pool, 64);
  ready = 1;
  rng_event(1);
  crypto_blake2b_update(&pool, (const uint8_t *)&now, sizeof(now));
  if (seedfile && (f = fopen(seedfile, "rb")))
  {
    size_t n = fread(buf, 1, sizeof(buf), f);
    crypto_blake2b_update(&pool, buf, n);
    fclose(f);
  }
  f = fopen("/dev/urandom", "rb");
  if (f)
  {
    size_t n = fread(buf, 1, sizeof(buf), f);
    crypto_blake2b_update(&pool, buf, n);
    fclose(f);
  }
  jitter(4096);
  reseed();
  rng_save(seedfile);
  crypto_wipe(buf, sizeof(buf));
}

void rng_bytes(void *out, size_t n)
{
  uint8_t next[32], nb[8];
  int i;

  rng_event(2);
  reseed();
  for (i = 0; i < 8; i++)
    nb[i] = (uint8_t)(nonce >> (i * 8));
  nonce++;
  memset(out, 0, n);
  crypto_chacha20_djb(out, out, n, key, nb, 1);
  memset(next, 0, sizeof(next));
  crypto_chacha20_djb(next, next, sizeof(next), key, nb, 0);
  memcpy(key, next, sizeof(key));       /* fast key erasure */
  crypto_wipe(next, sizeof(next));
}

void rng_save(const char *seedfile)
{
  uint8_t s[32];
  FILE *f;

  if (!seedfile)
    return;
  rng_bytes(s, sizeof(s));
  f = fopen(seedfile, "wb");
  if (f)
  {
    fwrite(s, 1, sizeof(s), f);
    fclose(f);
  }
  crypto_wipe(s, sizeof(s));
}
