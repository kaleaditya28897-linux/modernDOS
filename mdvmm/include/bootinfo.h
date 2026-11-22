/* MDVMM boot information, passed from a loader to the core.
 * Must match bootinfo.inc. The DOS loader fills it in conventional
 * memory; a future UEFI loader fills the same structure. */
#ifndef MDVMM_BOOTINFO_H
#define MDVMM_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC   0x4D56444Du    /* "MDVM" */

/* V86 breakpoints: HLT instructions at stub_seg:stub_bp + n */
#define BP_DPMI_ENTRY  0
#define BP_RM_RETURN   1
#define BP_RAW_TO_PM   2
#define BP_CALLBACK    3        /* 16 real mode callbacks */
#define BP_COUNT       19
#define BOOTINFO_VERSION 5

#define BOOT_DOS   0        /* started from DOS: real BIOS, DOS running */
#define BOOT_UEFI  1        /* started from UEFI: no BIOS, core boots DOS */

#define BOOTF_DISK_WRITE 0x1   /* UEFI: native hard disks may be written */

#define FB_RGBX    0        /* GOP PixelRedGreenBlueReserved8BitPerColor */
#define FB_BGRX    1        /* GOP PixelBlueGreenRedReserved8BitPerColor */

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
  uint16_t stub_bp;         /* offset of V86 breakpoints (HLT bytes) */
  uint16_t stub_entry;      /* offset of the DPMI mode switch entry */
  uint16_t stub_psp;        /* offset of word: PSP saved by the entry */
  uint16_t stub_retf;       /* offset of a RETF (no-op state save) */
  /* version 3 */
  uint32_t boot_type;       /* BOOT_DOS / BOOT_UEFI */
  uint32_t fb_phys;         /* UEFI: GOP framebuffer, 32 bpp */
  uint32_t fb_size;
  uint32_t fb_width, fb_height;
  uint32_t fb_pitch;        /* pixels per scan line */
  uint32_t fb_format;       /* FB_RGBX / FB_BGRX */
  uint32_t ramdisk_phys;    /* UEFI: boot disk image (floppy) */
  uint32_t ramdisk_size;
  uint32_t conv_kb;         /* UEFI: usable conventional memory */
  uint32_t rsdp;            /* UEFI: ACPI RSDP (0 = none) */
  /* version 4 */
  uint32_t xms_phys;        /* UEFI: DOS extended memory (from 1MB on, */
  uint32_t xms_size;        /* HMA first), physically contiguous */
  /* version 5 */
  uint32_t flags;           /* BOOTF_* */
} __attribute__((packed));

#endif
