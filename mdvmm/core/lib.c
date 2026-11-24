/* MDVMM core - minimal C library and logging (QEMU/Bochs debug port E9h) */
#include "core.h"

void *memcpy(void *d, const void *s, size_t n)
{
  uint8_t *dp = d;
  const uint8_t *sp = s;
  while (n--)
    *dp++ = *sp++;
  return d;
}

void *memset(void *d, int c, size_t n)
{
  uint8_t *dp = d;
  while (n--)
    *dp++ = (uint8_t)c;
  return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
  const uint8_t *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y)
      return *x - *y;
  return 0;
}

size_t strlen(const char *s)
{
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

static void putc_log(char c)
{
  if (c == '\n')
    outb(0xe9, '\r');
  outb(0xe9, (uint8_t)c);
  fbcon_log(c);                         /* UEFI boots: on the screen too */
}

static void put_num(uint32_t v, unsigned base, int width, char pad, int neg)
{
  char buf[12];
  int n = 0;
  do
  {
    buf[n++] = "0123456789ABCDEF"[v % base];
    v /= base;
  } while (v);
  if (neg)
    buf[n++] = '-';
  while (width-- > n)
    putc_log(pad);
  while (n)
    putc_log(buf[--n]);
}

/* supports %s %c %d %u %x %X %p with optional 0 padding and width */
void kvprintf(const char *fmt, va_list ap)
{
  for (; *fmt; fmt++)
  {
    char pad = ' ';
    int width = 0;

    if (*fmt != '%')
    {
      putc_log(*fmt);
      continue;
    }
    fmt++;
    if (*fmt == '0')
    {
      pad = '0';
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9')
      width = width * 10 + (*fmt++ - '0');
    switch (*fmt)
    {
      case 's':
      {
        const char *s = va_arg(ap, const char *);
        while (*s)
          putc_log(*s++);
        break;
      }
      case 'c':
        putc_log((char)va_arg(ap, int));
        break;
      case 'd':
      {
        int v = va_arg(ap, int);
        put_num(v < 0 ? -(uint32_t)v : (uint32_t)v, 10, width, pad, v < 0);
        break;
      }
      case 'u':
        put_num(va_arg(ap, uint32_t), 10, width, pad, 0);
        break;
      case 'x':
      case 'X':
      case 'p':
        put_num(va_arg(ap, uint32_t), 16, width, pad, 0);
        break;
      default:
        putc_log(*fmt);
    }
  }
}

void kprintf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  kvprintf(fmt, ap);
  va_end(ap);
}

static void halt_forever(void)
{
  for (;;)
    __asm__ volatile("cli; hlt");
}

void panic(const char *fmt, ...)
{
  va_list ap;
  fbcon_panic();
  kprintf("\nMDVMM PANIC: ");
  va_start(ap, fmt);
  kvprintf(fmt, ap);
  va_end(ap);
  kprintf("\n");
  halt_forever();
  __builtin_unreachable();
}

void panic_frame(struct frame *f, const char *why)
{
  fbcon_panic();
  kprintf("\nMDVMM PANIC: %s (vector %u, error %08x)\n", why, f->vector, f->err);
  kprintf("  EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n", f->eax, f->ebx, f->ecx, f->edx);
  kprintf("  ESI=%08x EDI=%08x EBP=%08x CR2=%08x\n", f->esi, f->edi, f->ebp, read_cr2());
  kprintf("  EIP=%08x CS=%04x EFLAGS=%08x\n", f->eip, f->cs, f->eflags);
  if (f->eflags & EFL_VM)
  {
    const uint8_t *code = rm_ptr(f->cs, f->eip);
    int i;
    kprintf("  V86 SS:SP=%04x:%04x DS=%04x ES=%04x FS=%04x GS=%04x\n",
            f->ss, f->esp & 0xffff, f->ds, f->es, f->fs, f->gs);
    kprintf("  code:");
    for (i = 0; i < 12; i++)
      kprintf(" %02x", code[i]);
    kprintf("\n");
  }
  halt_forever();
  __builtin_unreachable();
}
