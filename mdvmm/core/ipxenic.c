/* MDVMM core - network cards through iPXE's drivers
 *
 * mdvmm/ipxe builds iPXE's PCI network drivers into one object with a
 * handful of entry points (ipxe_*, glue.c there). This file gives that
 * object what it needs from the monitor (log, device memory, delays) and
 * registers each network device it finds as a `struct nic`.
 */
#include "core.h"

int ipxe_init(void);
int ipxe_get(int index, char *name, unsigned char *mac);
int ipxe_send(int index, const void *frame, int len);
int ipxe_recv(int index, void *frame, int room);
int ipxe_link(int index);
void ipxe_debug(int index);
void ipxe_set_virt_offset(unsigned long offset);

#define MAX_CARDS 4

struct card {
  struct nic nic;
  int index;
  char name[32];
};

static struct card cards[MAX_CARDS];

/* services for the iPXE object */
void mdvmm_log(const char *text);
void *mdvmm_ioremap(unsigned long phys, unsigned long len);
void mdvmm_udelay(unsigned long usecs);

void mdvmm_log(const char *text)
{
  kprintf("%s\n", text);
}

void *mdvmm_ioremap(unsigned long phys, unsigned long len)
{
  return mmio_map(phys, len);
}

void mdvmm_udelay(unsigned long usecs)
{
  udelay(usecs);
}

/* milliseconds since the start, from the time stamp counter (calibrated
   against the 8254 in ipxenic_init): iPXE's timeouts need a clock that
   runs while the monitor does not take interrupts */
static uint32_t tsc_per_ms;
static uint64_t tsc_start;

static uint64_t rdtsc(void)
{
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)hi << 32 | lo;
}

unsigned long mdvmm_ms(void);
unsigned long mdvmm_ms(void)
{
  uint64_t d = rdtsc() - tsc_start;
  uint32_t lo = (uint32_t)d, hi = (uint32_t)(d >> 32), q, r;

  if (!tsc_per_ms || hi >= tsc_per_ms)
    return 0;                           /* not calibrated (or 49 days on) */
  __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(tsc_per_ms));
  (void)r;
  return q;
}

static int c_send(struct nic *n, const void *frame, int len)
{
  return ipxe_send(((struct card *)n)->index, frame, len);
}

static int c_recv(struct nic *n, void *frame, int room)
{
  return ipxe_recv(((struct card *)n)->index, frame, room);
}

static int c_link(struct nic *n)
{
  return ipxe_link(((struct card *)n)->index);
}

static void c_debug(struct nic *n)
{
  ipxe_debug(((struct card *)n)->index);
}

void ipxenic_init(void)
{
  int n, i;

  /* the clock: time stamp counter ticks in two timer ticks (109.85 ms) */
  tsc_start = rdtsc();
  if (pit_wait(2))
  {
    uint64_t d = rdtsc() - tsc_start;
    uint32_t q, r;
    d = d * 100;                        /* / 10985 = per ms */
    __asm__("divl %4" : "=a"(q), "=d"(r) : "a"((uint32_t)d), "d"((uint32_t)(d >> 32)), "rm"((uint32_t)10985));
    (void)r;
    tsc_per_ms = q;
  }
  if (!tsc_per_ms)
    kprintf("ipxe: no clock (the 8254 does not tick): driver timeouts will not work\n");
  tsc_start = rdtsc();

  /* iPXE's memory model: physical = virtual + offset, true for the pool */
  ipxe_set_virt_offset(boot.pool_phys - KBASE);
  n = ipxe_init();
  for (i = 0; i < n && i < MAX_CARDS; i++)
  {
    struct card *c = &cards[i];
    c->index = i;
    if (!ipxe_get(i, c->name, c->nic.mac))
      continue;
    c->nic.name = c->name;
    c->nic.send = c_send;
    c->nic.recv = c_recv;
    c->nic.link = c_link;
    c->nic.debug = c_debug;
    nic_register(&c->nic);
  }
}
