/* MDVMM core: linked at KBASE, loaded anywhere (4K aligned) by a loader */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0xF0000000;
  .text : { *(.text.entry) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  . = ALIGN(16);
  __image_end = .;
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss*) *(COMMON)
    . = ALIGN(4096);
    __bss_end = .;
  }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
