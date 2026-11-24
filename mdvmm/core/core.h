/* MDVMM core - shared definitions */
#ifndef MDVMM_CORE_H
#define MDVMM_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "bootinfo.h"

/* Linear address space of the monitor:
 *   00000000-0010FFFF  V86 machine (DOS), user
 *   00400000-BEFFFFFF  DPMI client memory (dpmi.c CLIENT_BASE/END)
 *   C0000000-DEFFFFFF  DPMI 0800h physical mappings (PHYSMAP_BASE/END)
 *   E0000000-E0FFFFFF  DPMI host code page and stacks (HOST_AREA)
 *   E1000000-E7FFFFFF  UEFI framebuffer (FB_VIRT)
 *   E8000000-EBFFFFFF  UEFI boot disk image (RD_VIRT)
 *   EC000000-EDFFFFFF  device registers (MMIO_VIRT, pci.c)
 *   EE000000-EE3FFFFF  DOS session instance data (vm.c INST_BASE)
 *   EFC00000-EFFFFFFF  temporary physical windows (mm.c KMAP_BASE)
 *   F0000000-          the core and its pool (KBASE)
 *   FFC00000-FFFFFFFF  page tables (recursive mapping)
 */
#define KBASE     0xF0000000u
#define FB_VIRT   0xE1000000u
#define FB_MAX    0x07000000u
#define RD_VIRT   0xE8000000u
#define RD_MAX    0x04000000u
#define MMIO_VIRT 0xEC000000u
#define MMIO_MAX  0x02000000u
#define KCODE     0x08
#define KDATA     0x10
#define TSS_SEL   0x18
#define LDT_SEL   0x20
#define HCODE_SEL 0x2b          /* ring 3 host code (PM breakpoints)  */
#define HDATA_SEL 0x33          /* ring 3 host data (host PM stacks)  */
#define ESPFIX_SEL 0x38         /* ring 0 alias stack for IRET to 16 bit SS */

#define EFL_CF    0x00000001u
#define EFL_ZF    0x00000040u
#define EFL_TF    0x00000100u
#define EFL_IF    0x00000200u
#define EFL_OF    0x00000800u
#define EFL_IOPL  0x00003000u
#define EFL_NT    0x00004000u
#define EFL_RF    0x00010000u
#define EFL_VM    0x00020000u
#define EFL_AC    0x00040000u
#define EFL_ID    0x00200000u
/* flags a V86 program may change directly */
#define EFL_USER  0x00000DD5u   /* CF PF AF ZF SF TF DF OF */

/* Trap frame, built by isr_common (entry.asm). esp/ss are only present for
 * traps from ring 3 or V86, es/ds/fs/gs only for traps from V86 mode
 * (never touch them for other frames: they lie above the stack). */
struct frame {
  uint32_t pgs, pfs, pes, pds;                         /* PM segment regs */
  uint32_t edi, esi, ebp, esp_k, ebx, edx, ecx, eax;   /* pushad */
  uint32_t vector, err;
  uint32_t eip, cs, eflags;
  uint32_t esp, ss;
  uint32_t es, ds, fs, gs;
};

/* 16 bit register views */
#define AX(f) (*(uint16_t *)&(f)->eax)
#define BX(f) (*(uint16_t *)&(f)->ebx)
#define CX(f) (*(uint16_t *)&(f)->ecx)
#define DX(f) (*(uint16_t *)&(f)->edx)
#define SI(f) (*(uint16_t *)&(f)->esi)
#define DI(f) (*(uint16_t *)&(f)->edi)
#define AL(f) (*(uint8_t *)&(f)->eax)
#define AH(f) (*((uint8_t *)&(f)->eax + 1))
#define BL(f) (*(uint8_t *)&(f)->ebx)
#define BH(f) (*((uint8_t *)&(f)->ebx + 1))
#define CL(f) (*(uint8_t *)&(f)->ecx)
#define CH(f) (*((uint8_t *)&(f)->ecx + 1))
#define DL(f) (*(uint8_t *)&(f)->edx)
#define DH(f) (*((uint8_t *)&(f)->edx + 1))

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void invlpg(uint32_t v) { __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory"); }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile("mov %%cr2,%0" : "=r"(v)); return v; }

/* real mode / V86 address helpers (first 1MB+64K is identity mapped) */
static inline void *rm_ptr(uint32_t seg, uint32_t off) { return (void *)((seg << 4) + (off & 0xffff)); }

/* lib.c */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
void kvprintf(const char *fmt, va_list ap);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void panic_frame(struct frame *f, const char *why) __attribute__((noreturn));

/* mm.c */
extern struct bootinfo boot;
void mm_init(uint32_t free_phys);
uint32_t page_alloc(void);                   /* 0 if none */
uint32_t page_alloc_isa(uint32_t size);
void page_free(uint32_t phys);
uint32_t pages_free(void);
uint32_t pages_total(void);
int map_page(uint32_t lin, uint32_t phys, uint32_t flags);
int map_large_mmio(uint32_t lin, uint64_t phys);
uint32_t unmap_page(uint32_t lin);
uint32_t lin_to_pte(uint32_t lin);
void *kmap(int slot, uint32_t phys);
void *pool_ptr(uint32_t phys);              /* pool page, mapped at KBASE */
void phys_copy(uint32_t dst, uint32_t src, uint32_t len);

/* cpu.c */
void cpu_init(void);
void io_trap(uint16_t port, int trap);
void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);
void ldt_load(void *base, uint32_t entries);
const void *gdt_entry(uint16_t sel);

/* v86.c */
struct vm {
  uint32_t vif;             /* virtual interrupt flag */
  uint32_t vflags;          /* virtual IOPL/NT bits */
  uint32_t pending;         /* pending IRQ bitmap */
};
extern struct vm vm1;
void v86_start(void);
void v86_exception(struct frame *f);
void v86_irq(struct frame *f, int irq);
void v86_reflect(struct frame *f, uint8_t vec);
void v86_deliver(struct frame *f);
int v86_intercept(struct frame *f, uint8_t vec);
void v86_push16(struct frame *f, uint16_t v);
uint16_t v86_pop16(struct frame *f);
uint32_t v86_vflags(struct frame *f);

/* dpmi.c */
void dpmi_init(void);
void dpmi_detect(struct frame *v);
void dpmi_v86_bp(struct frame *v, int bp);
void dpmi_trap(struct frame *f);
void dpmi_irq(struct frame *f, int irq);
int dpmi_irq_v86(struct frame *v, int irq);
int dpmi_take_pending(struct frame *pm);
void dpmi_rm_exit(void);
int dpmi_active(void);
int dpmi_owner(void);
void dpmi_hwirq(struct frame *f, int irq);
int vm_current(void);
extern int vm_hold;

/* vm.c */
#define VRAM_PAGES 8
extern uint32_t vram_phys[VRAM_PAGES];
void vm_tick(struct frame *f);
void vm_poll(struct frame *f);
void vm_yield(struct frame *f);
int vm_kbd(struct frame *f);
int vm_port60(uint8_t *v);
void vm_key(uint8_t sc);
int dos_current_psp(void);
int vm_key_pending(void);
int vm_api(struct frame *f);
void vm_crit(struct frame *f);

/* bios.c, fbcon.c (UEFI boot: virtual PC BIOS) */
#define ROM_SEG 0xF000
void bios_init(void);
int bios_bp(struct frame *f, uint16_t ip);
int bios_mouse_install(void);
void apic_setup(void);
void timer_selftest(void);
int pit_wait(uint32_t ticks);
void fbcon_init(void);
void fbcon_text(void);
void fbcon_log(char c);
void fbcon_panic(void);
uint32_t fbcon_log_copy(char *dst, uint32_t room, uint32_t lines);
void fbcon_update(void);
void fbcon_graphics(int on);
void fbcon_clear(void);
void serial_putc(uint8_t c);
uint32_t ext_phys(uint32_t a);

/* pci.c */
uint32_t pci_read32(int bus, int dev, int fn, int reg);
void pci_write32(int bus, int dev, int fn, int reg, uint32_t v);
void pci_scan(uint32_t cls, void (*found)(int bus, int dev, int fn));
void pci_enable(int bus, int dev, int fn);
void *mmio_map(uint64_t phys, uint32_t size);
uint64_t pci_bar(int bus, int dev, int fn, int reg);
void udelay(uint32_t us);

/* hard disks for the virtual BIOS (INT 13h drives 80h+), from native
   drivers. rw() moves up to DISK_MAX_SECTORS 512-byte sectors between
   the disk and the pages in pages[] (4K each); 0 on success, else an
   INT 13h error code. flush() (optional) commits the drive's write
   cache; INT 13h disk resets call it. */
#define DISK_MAX_SECTORS 128
struct disk {
  uint64_t sectors;
  char model[41];
  void *drv;
  int (*rw)(struct disk *d, uint64_t lba, uint32_t n, const uint32_t *pages, int write);
  int (*flush)(struct disk *d);
};
void disk_register(struct disk *d);
void ahci_init(void);
void nvme_init(void);

/* xhci.c, usbkbd.c: USB keyboards */
void xhci_init(void);
void xhci_poll(void);
void uhci_init(void);
void uhci_poll(void);
void usbkbd_report(const uint8_t *report);
void usbkbd_tick(void);

/* mouse.c: INT 33h on UEFI boots */
void mouse_input(int buttons, int dx, int dy);
int mouse_text_cell(void);
void int33(struct frame *f);

/* nic.c, e1000.c: network cards (DOS and UEFI boots) */
struct nic {
  const char *name;
  uint8_t mac[6];
  int (*send)(struct nic *n, const void *frame, int len);     /* 0 ok */
  int (*recv)(struct nic *n, void *frame, int room);   /* len, 0 none, -1 dropped */
  int (*link)(struct nic *n);
  void (*debug)(struct nic *n);         /* optional: its state to the log */
};
void nic_register(struct nic *n);
void nic_init(void);
void nic_api(struct frame *f);
void e1000_init(void);
void yukon2_init(void);

/* dma.c */
void dma_init(void);
int dma_port(uint16_t port);
uint8_t dma_in(uint16_t port);
void dma_out(uint16_t port, uint8_t v);
void dma_irq(void);

/* entry.asm */
extern const uint32_t isr_table[256];
extern uint8_t kstack_top[];
void gdt_load(void *gdtr);
void enter_frame(const struct frame *f) __attribute__((noreturn));
_Static_assert(sizeof(struct frame) == 92, "FRAME_SIZE in entry.asm");

#endif
