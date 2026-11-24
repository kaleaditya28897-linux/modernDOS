#ifndef CONFIG_DEFAULTS_MDVMM_H
#define CONFIG_DEFAULTS_MDVMM_H
/* iPXE drivers inside the MDVMM monitor: flat 32 bit, pool memory at a
   fixed offset from its physical address, direct I/O, polled */
#define UACCESS_OFFSET
#define IOAPI_X86
#define PCIAPI_DIRECT
#define DMAAPI_FLAT
#define NAP_NULL
#define FDT_NULL
#endif
