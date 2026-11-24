/* partial link of the iPXE drivers: one section of each kind, the linker
   tables (drivers, ...) sorted into the data */
SECTIONS
{
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) KEEP(*(SORT(.tbl.*))) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  /DISCARD/ : { *(.einfo*) *(.comment) *(.note*) *(.discard*) *(.eh_frame*) }
}
