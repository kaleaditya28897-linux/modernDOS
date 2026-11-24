/* iPXE network drivers inside the MDVMM monitor: the glue
 *
 * iPXE (https://ipxe.org, GPL) has drivers for most PCI network cards,
 * written for a polled, single-threaded environment like this monitor.
 * build.sh compiles the drivers named in drivers.list and the few parts
 * of iPXE's core they need (network device, I/O buffers, heap, PCI) with
 * the "mdvmm" platform of overlay/, and this file:
 *   - what iPXE expects from a platform: mapping device memory, delays,
 *     debug output; stubs for subsystems that are not here (settings,
 *     timers)
 *   - what the monitor calls (ipxe_*, see core/ipxenic.c): probe the PCI
 *     network cards, send and receive Ethernet frames
 * Everything else in the linked object is local to it (objcopy), so
 * iPXE's names do not meet the monitor's.
 *
 * Memory: iPXE's 32-bit model is "physical = virtual + virt_offset". The
 * monitor's pool, which holds iPXE's code, data and heap, is mapped at a
 * fixed offset from its physical address, so that is exactly true.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <ipxe/netdevice.h>
#include <ipxe/ethernet.h>
#include <ipxe/if_ether.h>
#include <ipxe/iobuf.h>
#include <ipxe/malloc.h>
#include <ipxe/pci.h>
#include <ipxe/io.h>
#include <ipxe/settings.h>
#include <ipxe/retry.h>
#include <ipxe/errstat.h>
#include <ipxe/timer.h>
#include <ipxe/time.h>
#include <ipxe/nvo.h>
#include <ipxe/init.h>

/* the monitor's side (core/ipxenic.c) */
extern void mdvmm_log ( const char *text );
extern void * mdvmm_ioremap ( unsigned long phys, unsigned long len );
extern void mdvmm_udelay ( unsigned long usecs );
extern unsigned long mdvmm_ms ( void );

#define MAX_DEVS 4

static struct net_device *devs[MAX_DEVS];
static int ndevs;

/* ---------------------------------------------------------------- */
/* platform services for iPXE                                       */

void * ioremap ( unsigned long bus_addr, size_t len ) {
	return mdvmm_ioremap ( bus_addr, len );
}

void iounmap ( volatile const void *io __unused ) {
	/* mappings stay */
}

void udelay ( unsigned long usecs ) {
	mdvmm_udelay ( usecs );
}

void mdelay ( unsigned long msecs ) {
	while ( msecs-- )
		mdvmm_udelay ( 1000 );
}

/* iPXE's clock: TICKS_PER_SEC (1024) ticks per second */
unsigned long currticks ( void ) {
	return ( mdvmm_ms() + ( mdvmm_ms() >> 6 ) );
}

int errno;

/* debug and console output: whole lines to the monitor's log */
static char line[200];
static size_t line_len;

int putchar ( int character ) {
	if ( character == '\n' || line_len == sizeof ( line ) - 1 ) {
		line[line_len] = 0;
		mdvmm_log ( line );
		line_len = 0;
	}
	if ( character != '\n' && character != '\r' )
		line[line_len++] = ( char ) character;
	return character;
}

void dbg_printf ( const char *fmt, ... ) {
	char buf[256];
	va_list args;
	char *c;

	va_start ( args, fmt );
	vsnprintf ( buf, sizeof ( buf ), fmt, args );
	va_end ( args );
	for ( c = buf ; *c ; c++ )
		putchar ( *c );
}

void dbg_autocolourise ( unsigned long id __unused ) {
}

void dbg_hex_dump_da ( unsigned long dispaddr __unused,
		       const void *data __unused, unsigned long len __unused ) {
}

void dbg_md5_da ( unsigned long dispaddr __unused, const void *data __unused,
		  unsigned long len __unused ) {
}

void dbg_pause ( void ) {
}

void dbg_more ( void ) {
}

/* not here: settings, retry timers, error statistics, the clock */
struct settings_operations netdev_settings_operations;

int register_settings ( struct settings *settings __unused,
			struct settings *parent __unused,
			const char *name __unused ) {
	return 0;
}

void unregister_settings ( struct settings *settings __unused ) {
}

void clear_settings ( struct settings *settings __unused ) {
}

void start_timer_fixed ( struct retry_timer *timer __unused,
			 unsigned long timeout __unused ) {
}

void stop_timer ( struct retry_timer *timer __unused ) {
}

void record_stat ( struct error_statistics *stats __unused, int rc __unused ) {
}

/* options stored in a card's EEPROM: not used */
void nvo_init ( struct nvo_block *nvo __unused, struct nvs_device *nvs __unused,
		size_t address __unused, size_t len __unused,
		int ( * resize ) ( struct nvo_block *nvo, size_t len ) __unused,
		struct refcnt *refcnt __unused ) {
}

int register_nvo ( struct nvo_block *nvo __unused,
		   struct settings *parent __unused ) {
	return 0;
}

void unregister_nvo ( struct nvo_block *nvo __unused ) {
}

signed long time_offset;

time_t time_now ( void ) {
	return 0;
}

/* ---------------------------------------------------------------- */
/* for the monitor                                                  */

static struct device pci_root = {
	.name = "PCI",
	.siblings = LIST_HEAD_INIT ( pci_root.siblings ),
	.children = LIST_HEAD_INIT ( pci_root.children ),
};

/**
 * Find the PCI network cards that have a driver and start them
 *
 * @ret count		Number of network devices
 */
int ipxe_init ( void ) {
	struct pci_device *pci = NULL;
	struct net_device *netdev;
	uint32_t busdevfn = 0;
	struct init_fn *init_fn;
	char msg[160];
	int rc;

	/* iPXE's initialisation functions (the heap, ...) */
	for_each_table_entry ( init_fn, INIT_FNS )
		init_fn->initialise();

	do {
		if ( ! pci )
			pci = malloc ( sizeof ( *pci ) );
		if ( ! pci )
			break;
		if ( pci_find_next ( pci, &busdevfn ) != 0 )
			break;
		if ( ( pci->class >> 16 ) != 0x02 )
			continue;	/* network controllers only */
		if ( pci_find_driver ( pci ) != 0 ) {
			snprintf ( msg, sizeof ( msg ), "ipxe: " PCI_FMT
				   " network card %04x:%04x: no driver",
				   PCI_ARGS ( pci ), pci->vendor, pci->device );
			mdvmm_log ( msg );
			continue;
		}
		snprintf ( msg, sizeof ( msg ), "ipxe: " PCI_FMT " %04x:%04x: "
			   "driver for %s", PCI_ARGS ( pci ), pci->vendor,
			   pci->device, pci->id->name );
		mdvmm_log ( msg );
		pci->dev.parent = &pci_root;
		list_add ( &pci->dev.siblings, &pci_root.children );
		if ( ( rc = pci_probe ( pci ) ) == 0 ) {
			pci = NULL;
		} else {
			snprintf ( msg, sizeof ( msg ), "ipxe: the driver "
				   "could not start the card (error %08x)",
				   -rc );
			mdvmm_log ( msg );
			list_del ( &pci->dev.siblings );
		}
	} while ( ++busdevfn );
	free ( pci );

	for_each_netdev ( netdev ) {
		if ( ndevs == MAX_DEVS )
			break;
		if ( ( rc = netdev_open ( netdev ) ) != 0 ) {
			snprintf ( msg, sizeof ( msg ), "ipxe: %s: cannot "
				   "open (error %08x)", netdev->name, -rc );
			mdvmm_log ( msg );
			continue;
		}
		devs[ndevs++] = netdev;
	}
	return ndevs;
}

/**
 * Describe a network device
 *
 * @v index		Device number
 * @v name		Buffer for the driver's name (32 bytes)
 * @v mac		Buffer for the MAC address (6 bytes)
 * @ret ok		The device exists
 */
int ipxe_get ( int index, char *name, unsigned char *mac ) {
	struct net_device *netdev;

	if ( index < 0 || index >= ndevs )
		return 0;
	netdev = devs[index];
	snprintf ( name, 32, "%s (iPXE)", netdev->dev->driver_name );
	memcpy ( mac, netdev->ll_addr, ETH_ALEN );
	return 1;
}

/** Send an Ethernet frame; 0 on success */
int ipxe_send ( int index, const void *frame, int len ) {
	struct net_device *netdev = devs[index];
	struct io_buffer *iobuf;
	int rc;

	iobuf = alloc_iob ( len < ETH_ZLEN ? ETH_ZLEN : len );
	if ( ! iobuf )
		return -1;
	memcpy ( iob_put ( iobuf, len ), frame, len );
	if ( len < ETH_ZLEN )
		memset ( iob_put ( iobuf, ETH_ZLEN - len ), 0, ETH_ZLEN - len );
	rc = netdev_tx ( netdev, iobuf );	/* takes the buffer */
	netdev_poll ( netdev );
	return ( rc ? -1 : 0 );
}

/** Receive the next frame; its length, 0 if there is none */
int ipxe_recv ( int index, void *frame, int room ) {
	struct net_device *netdev = devs[index];
	struct io_buffer *iobuf;
	int len;

	netdev_poll ( netdev );
	iobuf = netdev_rx_dequeue ( netdev );
	if ( ! iobuf )
		return 0;
	len = iob_len ( iobuf );
	if ( len <= room )
		memcpy ( frame, iobuf->data, len );
	else
		len = 0;
	free_iob ( iobuf );
	return len;
}

/** Is the link up? */
int ipxe_link ( int index ) {
	netdev_poll ( devs[index] );
	return netdev_link_ok ( devs[index] );
}

/** The device's state, to the monitor's log */
void ipxe_debug ( int index ) {
	struct net_device *netdev = devs[index];
	char msg[200];

	netdev_poll ( netdev );
	snprintf ( msg, sizeof ( msg ), "ipxe: %s %s: link %s (%08x), tx %d "
		   "ok %d bad, rx %d ok %d bad, state %x", netdev->name,
		   netdev->dev->driver_name,
		   ( netdev_link_ok ( netdev ) ? "up" : "down" ),
		   -netdev->link_rc, netdev->tx_stats.good,
		   netdev->tx_stats.bad, netdev->rx_stats.good,
		   netdev->rx_stats.bad, netdev->state );
	mdvmm_log ( msg );
}
