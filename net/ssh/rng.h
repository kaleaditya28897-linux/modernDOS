#ifndef RNG_H
#define RNG_H
#include <stddef.h>
#include <stdint.h>

void rng_init(const char *seedfile);    /* seedfile may be NULL */
void rng_event(uint32_t what);          /* mix in the time of an event */
void rng_bytes(void *out, size_t n);
void rng_save(const char *seedfile);    /* new seed for the next run */

#endif
