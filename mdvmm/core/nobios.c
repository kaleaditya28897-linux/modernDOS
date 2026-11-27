/* MDVMM core - stubs for the core linked into MDVMM.COM. Started from
   DOS, the machine has a PC BIOS: the virtual BIOS and the native drivers
   for disks, screen and input (bios.c, fbcon.c, font.c, rom.asm, ahci.c,
   ...) are only linked into the UEFI loader's core. */
#include "core.h"

void bios_init(void)
{
  panic("this core has no virtual BIOS (use BOOTX64.EFI)");
}

int bios_bp(struct frame *f, uint16_t ip)
{
  (void)f;
  (void)ip;
  return 0;
}

uint32_t ext_phys(uint32_t a)
{
  return a;                             /* DOS boot: extended memory is physical */
}

int bios_mouse_install(void)
{
  return 0;                             /* DOS boot: load a mouse driver */
}

/* no framebuffer console: the BIOS has the screen */
void fbcon_init(void)
{
}

void fbcon_log(char c)
{
  (void)c;
}

void fbcon_panic(void)
{
}

uint32_t fbcon_log_copy(char *dst, uint32_t room, uint32_t lines)
{
  (void)dst;
  (void)room;
  (void)lines;
  return 0;                             /* the log is not kept on DOS boots */
}

/* VGA mode 13h: the machine's video BIOS and card do that */
int vga_active(void)
{
  return 0;
}

int vga_port_in(uint16_t port, int size, uint32_t *v)
{
  (void)port;
  (void)size;
  (void)v;
  return 0;
}

int vga_port_out(uint16_t port, int size, uint32_t v)
{
  (void)port;
  (void)size;
  (void)v;
  return 0;
}

int vga_debug(struct frame *f, const uint8_t *code, const uint8_t *esi_byte, int code32)
{
  (void)f;
  (void)code;
  (void)esi_byte;
  (void)code32;
  return 0;
}

int vga_fault(struct frame *f, uint32_t addr, int v86, int code32)
{
  (void)f;
  (void)addr;
  (void)v86;
  (void)code32;
  return 0;
}

void bios_program_end(void)
{
}
