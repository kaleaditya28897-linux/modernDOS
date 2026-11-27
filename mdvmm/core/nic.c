/* MDVMM core - network cards
 *
 * Drivers (iPXE's, through ipxenic.c) register a `struct nic`; DOS reaches the
 * first one through INT 2Fh AH=DEh (PKTDRV.COM turns that into a standard
 * packet driver):
 *   AX=DE10h  info: ES:DI <- 6-byte MAC, AX = MTU (frame bytes without
 *             CRC), BX = 1 if the link is up, CX = cards; CF if none
 *   AX=DE11h  send the frame at DS:SI, CX bytes (14..1514); CF on error
 *   AX=DE12h  receive: the next frame into ES:DI (CX = room); CX = its
 *             length, 0 if there is none
 *   AX=DE13h  the driver's state into the monitor's log
 *   AX=DE14h  the monitor's log: its last BX lines (0: all kept) as text
 *             into ES:DI (CX = room); CX = bytes (works without a card;
 *             UEFI boots only, MDLOG.COM shows it)
 * and the system calls of MDSYS.COM:
 *   AX=DE15h  state: AX bit 0 = UEFI boot, bit 1 = trial boot, bit 2 = trial
 *             confirmed
 *   AX=DE16h  confirm the trial boot (no automatic restart)
 *   AX=DE17h  restart the machine
 * Drivers poll (no interrupts), like the disk drivers. Works on DOS and
 * UEFI boots.
 */
#include "core.h"

#define MAX_NICS 4

static struct nic *nics[MAX_NICS];
static int nnic;

/* ---------------------------------------------------------------- */
/* One card, several DOS sessions, each with its own TCP/IP stack (the
   packet driver is loaded before the sessions are forked, a program's
   Watt-32 asks DHCP and gets the card's one address). Frames from the
   card are sorted here:
     - TCP and UDP of a connection a session has sent on: to that session
     - TCP to a port a session has accepted on (it sent SYN+ACK): there
     - everything else (ARP, DHCP, ICMP, a first SYN): to every session
       that polls
   and on the way out, the answers stacks give to segments that were not
   theirs are dropped: a TCP reset on a connection that is another
   session's, or - while several sessions are on the network - on one
   that is nobody's yet, and ICMP "unreachable". */
#define NQ        24                    /* frames queued per session */
#define NFLOWS    128
#define NLISTEN   16
#define POLL_AGE  40                    /* ticks: a session that polled is "on" */

struct nq {
  uint16_t len[NQ];
  uint8_t data[NQ][1514];
  int head, count;
  uint32_t last_poll;
  int polled;
};
struct flow {
  uint8_t used, proto, ses;
  uint16_t lport, rport;
  uint32_t rip, stamp;
};
static struct nq nq[MAX_SESSIONS];
static struct flow flows[NFLOWS];
static struct { uint16_t port; uint8_t used, ses; } listeners[NLISTEN];
static uint32_t flow_clock;

static int net_on(int s)
{
  return nq[s].polled && vm_ticks() - nq[s].last_poll < POLL_AGE;
}

static int sessions_on(void)
{
  int s, n = 0;

  for (s = 0; s < MAX_SESSIONS; s++)
    n += net_on(s);
  return n;
}

/* an IPv4 TCP or UDP frame: protocol, addresses and ports, the TCP flags */
struct l4 { uint8_t proto, flags; uint32_t sip, dip; uint16_t sport, dport; };

static int parse(const uint8_t *fr, int len, struct l4 *k)
{
  const uint8_t *ip = fr + 14, *t;
  int ihl;

  if (len < 14 + 20 + 8 || fr[12] != 0x08 || fr[13] != 0x00 || (ip[0] >> 4) != 4)
    return 0;
  ihl = (ip[0] & 15) * 4;
  k->proto = ip[9];
  if ((k->proto != 6 && k->proto != 17) || len < 14 + ihl + (k->proto == 6 ? 20 : 8))
    return 0;
  if ((ip[6] & 0x1f) || ip[7])
    return 0;                           /* not the first fragment */
  memcpy(&k->sip, ip + 12, 4);
  memcpy(&k->dip, ip + 16, 4);
  t = ip + ihl;
  k->sport = (uint16_t)(t[0] << 8 | t[1]);
  k->dport = (uint16_t)(t[2] << 8 | t[3]);
  k->flags = k->proto == 6 ? t[13] : 0;
  return 1;
}

static struct flow *flow_find(uint8_t proto, uint16_t lport, uint32_t rip, uint16_t rport)
{
  int i;

  for (i = 0; i < NFLOWS; i++)
    if (flows[i].used && flows[i].proto == proto && flows[i].lport == lport &&
        flows[i].rip == rip && flows[i].rport == rport)
      return &flows[i];
  return 0;
}

/* a frame a session sends: 0 if it must not go out */
static int outbound(int ses, const uint8_t *fr, int len)
{
  struct l4 k;
  struct flow *fl;
  int i, old = 0;

  if (len >= 14 + 20 + 8 && fr[12] == 0x08 && fr[13] == 0x00 && fr[14 + 9] == 1 &&
      fr[14 + (fr[14] & 15) * 4] == 3 && sessions_on() > 1)
    return 0;                           /* ICMP unreachable */
  if (!parse(fr, len, &k))
    return 1;
  if (k.proto == 17 && (k.sport == 68 || k.dport == 67))
    return 1;                           /* DHCP: every stack's own, answers to all */
  fl = flow_find(k.proto, k.sport, k.dip, k.dport);
  if (k.flags & 0x04)                   /* TCP reset */
  {
    if (fl ? fl->ses != ses : sessions_on() > 1)
      return 0;
    return 1;
  }
  if ((k.flags & 0x12) == 0x12)         /* SYN+ACK: this session accepts on the port */
  {
    for (i = 0; i < NLISTEN && listeners[i].used && listeners[i].port != k.sport; i++)
      ;
    if (i == NLISTEN)
      i = k.sport % NLISTEN;
    listeners[i].used = 1;
    listeners[i].port = k.sport;
    listeners[i].ses = (uint8_t)ses;
  }
  if (!fl)
  {
    for (i = 0; i < NFLOWS && flows[i].used; i++)
      if (flows[i].stamp < flows[old].stamp)
        old = i;
    fl = &flows[i < NFLOWS ? i : old];  /* a free one, else the oldest */
    fl->used = 1;
    fl->proto = k.proto;
    fl->lport = k.sport;
    fl->rip = k.dip;
    fl->rport = k.dport;
  }
  fl->ses = (uint8_t)ses;
  fl->stamp = ++flow_clock;
  return 1;
}

/* the session a received frame is for, -1: all that are on */
static int inbound(const uint8_t *fr, int len)
{
  struct l4 k;
  struct flow *fl;
  int i;

  if (!parse(fr, len, &k) || (k.proto == 17 && k.dport == 68))
    return -1;
  if ((fl = flow_find(k.proto, k.dport, k.sip, k.sport)))
  {
    fl->stamp = ++flow_clock;
    return fl->ses;
  }
  for (i = 0; i < NLISTEN && k.proto == 6; i++)
    if (listeners[i].used && listeners[i].port == k.dport)
      return listeners[i].ses;
  return -1;
}

static void enqueue(int s, const uint8_t *fr, int len)
{
  struct nq *q = &nq[s];
  int at;

  if (q->count == NQ)
    return;                             /* full: dropped, as on a busy card */
  at = (q->head + q->count++) % NQ;
  q->len[at] = (uint16_t)len;
  memcpy(q->data[at], fr, (uint32_t)len);
}

/* frames from the card into the sessions' queues */
static void pump(struct nic *n)
{
  static uint8_t fr[1514];
  int len, i, s, to;

  for (i = 0; i < 16 && (len = n->recv(n, fr, sizeof(fr))) > 0; i++)
  {
    to = inbound(fr, len);
    if (to >= 0 && !net_on(to))
      to = -1;                          /* its program is gone */
    for (s = 0; s < MAX_SESSIONS; s++)
      if (to < 0 ? net_on(s) : s == to)
        enqueue(s, fr, len);
  }
}

void nic_register(struct nic *n)
{
  if (nnic == MAX_NICS)
    return;
  nics[nnic++] = n;
  kprintf("nic: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", n->name, n->mac[0], n->mac[1],
          n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
}

void nic_init(void)
{
  if (boot.flags & BOOTF_NO_NET)
  {
    kprintf("nic: network drivers skipped (\\MDOS\\NONET)\n");
    return;
  }
  ipxenic_init();                       /* iPXE's drivers, on DOS and UEFI boots */
}

static void set_cf(struct frame *f, int on)
{
  if (on)
    f->eflags |= EFL_CF;
  else
    f->eflags &= ~EFL_CF;
}

/* INT 2Fh AX=DE10h-DE1Fh */
void nic_api(struct frame *f)
{
  struct nic *n = nnic ? nics[0] : 0;
  uint8_t *buf;
  int len;

  if (AL(f) == 0x15)
  {
    AX(f) = (uint16_t)((boot.boot_type == BOOT_UEFI) | (boot.flags & BOOTF_TRIAL ? 2 : 0) |
                       (trial_confirmed ? 4 : 0) |
                       (boot.boot_type != BOOT_UEFI || (boot.flags & BOOTF_DISK_WRITE) ? 8 : 0));
    set_cf(f, 0);
    return;
  }
  if (AL(f) == 0x16)
  {
    if ((boot.flags & BOOTF_TRIAL) && !trial_confirmed)
      kprintf("trial boot: confirmed\n");
    trial_confirmed = 1;
    set_cf(f, 0);
    return;
  }
  if (AL(f) == 0x17)
  {
    kprintf("restart requested\n");
    machine_reset();
  }
  if (AL(f) == 0x14)
  {
    CX(f) = (uint16_t)fbcon_log_copy((char *)rm_ptr(f->es, 0) + DI(f), CX(f), BX(f));
    set_cf(f, !CX(f));
    return;
  }
  if (!n)
  {
    set_cf(f, 1);
    return;
  }
  switch (AL(f))
  {
    case 0x10:
      memcpy(rm_ptr(f->es, DI(f)), n->mac, 6);
      AX(f) = 1514;
      BX(f) = (uint16_t)(n->link ? n->link(n) : 1);
      CX(f) = (uint16_t)nnic;
      set_cf(f, 0);
      return;
    case 0x11:
      len = CX(f);
      buf = rm_ptr(f->ds, 0) + SI(f);
      if (len < 14 || len > 1514)
      {
        kprintf("nic: send of %d bytes refused\n", len);
        set_cf(f, 1);
        return;
      }
      set_cf(f, outbound(vm_current(), buf, len) ? n->send(n, buf, len) : 0);
      return;
    case 0x12:
      buf = rm_ptr(f->es, 0) + DI(f);
    {
      struct nq *q = &nq[vm_current()];
      q->polled = 1;
      q->last_poll = vm_ticks();
      pump(n);
      len = 0;
      if (q->count)
      {
        len = q->len[q->head] <= CX(f) ? q->len[q->head] : 0;
        memcpy(buf, q->data[q->head], (uint32_t)len);
        q->head = (q->head + 1) % NQ;
        q->count--;
      }
      CX(f) = (uint16_t)len;
      set_cf(f, 0);
      return;
    }
    case 0x13:
      if (n->debug)
        n->debug(n);
      vm_debug();                       /* and where the sessions are */
      set_cf(f, 0);
      return;
  }
  set_cf(f, 1);
}
