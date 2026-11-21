/* MDVMM boot information, passed from a loader to the core.
 * Must match bootinfo.inc. The DOS loader fills it in conventional
 * memory; a future UEFI loader fills the same structure. */
#ifndef MDVMM_BOOTINFO_H
#define MDVMM_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC   0x4D56444Du    /* "MDVM" */
#define BOOTINFO_VERSION 1

struct bootinfo {
  uint32_t magic;
  uint32_t version;
  uint32_t pool_phys;       /* 4K aligned physical base; core loaded here */
  uint32_t pool_size;       /* bytes, multiple of 4K */
  uint32_t core_size;       /* bytes of core image at pool_phys */
  uint32_t trampoline;      /* linear address of the paging trampoline */
  uint32_t eflags;          /* V86 resume state */
  uint16_t ip, cs, sp, ss, ds, es, fs, gs;
  uint16_t psp;             /* loader PSP segment */
  uint16_t stub_seg;        /* segment of resident V86 stubs */
  uint16_t stub_dpmi;       /* offset of DPMI mode switch stub (M2.2) */
  uint16_t reserved;
} __attribute__((packed));

#endif
