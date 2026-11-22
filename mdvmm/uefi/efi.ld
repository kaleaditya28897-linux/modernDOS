/* BOOTX64.EFI: PE32+ from ELF objects (ld -m i386pep) */
SECTIONS
{
  . = 0x10001000;
  .text : { *(.text*) *(.rodata*) }
  . = ALIGN(4096);
  .data : { *(.data*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
