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
static int nic_session;                 /* the session of the packet driver */

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
      /* the session that asks (PKTDRV when it installs) gets the frames:
         copies of the packet driver in sessions forked later do not */
      nic_session = vm_current();
      memcpy(rm_ptr(f->es, DI(f)), n->mac, 6);
      AX(f) = 1514;
      BX(f) = (uint16_t)(n->link ? n->link(n) : 1);
      CX(f) = (uint16_t)nnic;
      set_cf(f, 0);
      return;
    case 0x11:
      len = CX(f);
      buf = rm_ptr(f->ds, 0) + SI(f);
      set_cf(f, len < 14 || len > 1514 || n->send(n, buf, len));
      if (len < 14 || len > 1514)
        kprintf("nic: send of %d bytes refused\n", len);
      return;
    case 0x12:
      buf = rm_ptr(f->es, 0) + DI(f);
      len = vm_current() == nic_session ? n->recv(n, buf, CX(f)) : 0;
      CX(f) = (uint16_t)(len > 0 ? len : 0);
      set_cf(f, 0);
      return;
    case 0x13:
      if (n->debug)
        n->debug(n);
      set_cf(f, 0);
      return;
  }
  set_cf(f, 1);
}
