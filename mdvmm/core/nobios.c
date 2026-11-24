/* MDVMM core - stubs for the core linked into MDVMM.COM. Started from
   DOS, the machine has a PC BIOS: the virtual BIOS and the native drivers
   (bios.c, fbcon.c, font.c, rom.asm, pci.c, ahci.c) are only linked into
   the UEFI loader's core, which keeps MDVMM.COM well below 64 KB. */
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
