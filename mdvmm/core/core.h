/* MDVMM core - shared definitions */
#ifndef MDVMM_CORE_H
#define MDVMM_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "bootinfo.h"

#define KBASE     0xF0000000u
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
uint32_t unmap_page(uint32_t lin);
uint32_t lin_to_pte(uint32_t lin);
void *kmap(int slot, uint32_t phys);
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

/* vm.c */
void vm_tick(struct frame *f);
void vm_poll(struct frame *f);
void vm_yield(struct frame *f);
int vm_kbd(struct frame *f);
int vm_port60(uint8_t *v);
int vm_api(struct frame *f);
void vm_crit(struct frame *f);

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
