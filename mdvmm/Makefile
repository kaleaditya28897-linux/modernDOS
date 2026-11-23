# MDVMM - modernDOS virtual machine monitor
# Builds build/MDVMM.COM (16-bit DOS loader with the 32-bit core embedded)
# and build/BOOTX64.EFI (UEFI loader with the same core embedded).

CC      = i686-linux-gnu-gcc
LD      = i686-linux-gnu-ld
OBJCOPY = i686-linux-gnu-objcopy
NASM    = nasm
CC64    = gcc
LD64    = ld

CFLAGS  = -m32 -std=gnu11 -O2 -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
          -fno-delete-null-pointer-checks -fno-strict-aliasing -fno-asynchronous-unwind-tables \
          -mgeneral-regs-only --param=min-pagesize=0 -Iinclude -Icore
NFLAGS  = -Iinclude/

B       = build
# core objects; the UEFI core adds the virtual BIOS and native drivers,
# the DOS core (MDVMM.COM, 64 KB limit) has stubs instead
COBJS   = $(B)/entry.o $(B)/main.o $(B)/lib.o $(B)/mm.o $(B)/cpu.o $(B)/v86.o $(B)/dpmi.o \
          $(B)/vm.o $(B)/dma.o
UEFIOBJS = $(B)/bios.o $(B)/fbcon.o $(B)/font.o $(B)/romimg.o $(B)/pci.o $(B)/ahci.o $(B)/nvme.o \
          $(B)/xhci.o $(B)/usbkbd.o $(B)/mouse.o
DOSOBJS = $(B)/nobios.o
HDRS    = core/core.h include/bootinfo.h

CFLAGS64 = -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
          -fpie -fno-stack-protector -mno-red-zone -mgeneral-regs-only \
          -fno-asynchronous-unwind-tables -fshort-wchar -Iinclude -Iuefi

all: $(B)/MDVMM.COM $(B)/BOOTX64.EFI

$(B):
	mkdir -p $(B)

$(B)/entry.o: core/entry.asm include/bootinfo.inc | $(B)
	$(NASM) $(NFLAGS) -f elf32 -o $@ $<

$(B)/rom.bin: core/rom.asm include/bootinfo.inc | $(B)
	$(NASM) $(NFLAGS) -f bin -o $@ $<

$(B)/romimg.o: core/romimg.asm $(B)/rom.bin
	$(NASM) $(NFLAGS) -I$(B)/ -f elf32 -o $@ $<

$(B)/%.o: core/%.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

LDCORE  = $(LD) -m elf_i386 -z noexecstack --no-warn-rwx-segments -T core/core.ld

$(B)/core.elf: $(COBJS) $(UEFIOBJS) core/core.ld
	$(LDCORE) -o $@ $(COBJS) $(UEFIOBJS)

$(B)/core_dos.elf: $(COBJS) $(DOSOBJS) core/core.ld
	$(LDCORE) -o $@ $(COBJS) $(DOSOBJS)

$(B)/%.bin: $(B)/%.elf
	$(OBJCOPY) -O binary $< $@

$(B)/MDVMM.COM: loader/loader.asm $(B)/core_dos.bin include/bootinfo.inc
	$(NASM) $(NFLAGS) -I$(B)/ -f bin -o $@ $<

$(B)/uefi_main.o: uefi/main.c uefi/efi.h include/bootinfo.h | $(B)
	$(CC64) $(CFLAGS64) -c -o $@ $<

$(B)/uefi_start.o: uefi/start.asm $(B)/core.bin
	$(NASM) -I$(B)/ -f elf64 -o $@ $<

$(B)/BOOTX64.EFI: $(B)/uefi_main.o $(B)/uefi_start.o uefi/efi.ld
	$(LD64) -m i386pep --subsystem 10 --image-base 0x10000000 -e efi_main --enable-reloc-section -nostdlib \
	  -T uefi/efi.ld -o $@ $(B)/uefi_main.o $(B)/uefi_start.o

clean:
	rm -rf $(B)

.PHONY: all clean
