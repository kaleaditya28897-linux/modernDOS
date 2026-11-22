; the virtual BIOS ROM image (rom.asm), copied to F0000h on UEFI boots
section .rodata
global rom_image, rom_image_end
rom_image:
        incbin  "rom.bin"
rom_image_end:
