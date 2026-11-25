# MDVMM - modernDOS virtual machine monitor
# Builds build/MDVMM.COM (16-bit DOS loader with the 32-bit core appended)
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
# core objects; the UEFI core adds the virtual BIOS and native drivers for
# disks, screen and input, the DOS core has stubs instead (the machine's
# BIOS does that); both have iPXE's network drivers
COBJS   = $(B)/entry.o $(B)/main.o $(B)/lib.o $(B)/mm.o $(B)/cpu.o $(B)/v86.o $(B)/dpmi.o \
          $(B)/vm.o $(B)/dma.o $(B)/pci.o $(B)/nic.o $(B)/ipxenic.o $(B)/ipxe.o
UEFIOBJS = $(B)/bios.o $(B)/fbcon.o $(B)/font.o $(B)/romimg.o $(B)/ahci.o $(B)/nvme.o \
          $(B)/xhci.o $(B)/usbkbd.o $(B)/mouse.o $(B)/apic.o $(B)/uhci.o
DOSOBJS = $(B)/nobios.o
HDRS    = core/core.h include/bootinfo.h

# iPXE's network drivers (ipxe/): the pinned source tree of the build image
IPXE    ?= $(firstword $(wildcard /opt/ipxe ../_output/ext/ipxe/src))
IPXE_DEBUG ?=

CFLAGS64 = -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
          -fpie -fno-stack-protector -mno-red-zone -mgeneral-regs-only \
          -fno-asynchronous-unwind-tables -fshort-wchar -Iinclude -Iuefi

all: $(B)/MDVMM.COM $(B)/BOOTX64.EFI $(B)/PKTDRV.COM $(B)/SSHSLOT.COM $(B)/MDLOG.COM $(B)/MDSYS.COM

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

# MDVMM.COM is an MZ program laid out like a .COM (loader/mz.asm), with
# the core appended: the loader reads it from its own file, so the core
# is not held to 64 KB
$(B)/loader.bin: loader/loader.asm include/bootinfo.inc | $(B)
	$(NASM) $(NFLAGS) -f bin -o $@ $<

$(B)/MDVMM.COM: loader/mz.asm $(B)/loader.bin $(B)/core_dos.bin
	$(NASM) -I$(B)/ -DLSIZE=$$(stat -c %s $(B)/loader.bin) -f bin -o $@ $<

$(B)/uefi_main.o: uefi/main.c uefi/efi.h uefi/loader.h include/bootinfo.h | $(B)
	$(CC64) $(CFLAGS64) -c -o $@ $<

$(B)/uefi_start.o: uefi/start.asm $(B)/core.bin
	$(NASM) -I$(B)/ -f elf64 -o $@ $<

$(B)/uefi_hwinfo.o: uefi/hwinfo.c uefi/efi.h | $(B)
	$(CC64) $(CFLAGS64) -c -o $@ $<

$(B)/uefi_install.o: uefi/install.c uefi/loader.h uefi/efi.h | $(B)
	$(CC64) $(CFLAGS64) -c -o $@ $<

$(B)/BOOTX64.EFI: $(B)/uefi_main.o $(B)/uefi_hwinfo.o $(B)/uefi_install.o $(B)/uefi_start.o uefi/efi.ld
	$(LD64) -m i386pep --subsystem 10 --image-base 0x10000000 -e efi_main --enable-reloc-section -nostdlib \
	  -T uefi/efi.ld -o $@ $(B)/uefi_main.o $(B)/uefi_hwinfo.o $(B)/uefi_install.o $(B)/uefi_start.o

$(B)/ipxe.o: ipxe/glue.c ipxe/glue_data.c ipxe/build.sh ipxe/drivers.list ipxe/core.list ipxe/ipxe.lds \
            ipxe/overlay/config/defaults/mdvmm.h | $(B)
	bash ipxe/build.sh $(IPXE) $@ $(IPXE_DEBUG)

$(B)/PKTDRV.COM: pktdrv/pktdrv.asm | $(B)
	$(NASM) -f bin -o $@ $<

$(B)/SSHSLOT.COM: sshslot/sshslot.asm | $(B)
	$(NASM) -f bin -o $@ $<

$(B)/MDLOG.COM: mdlog/mdlog.asm | $(B)
	$(NASM) -f bin -o $@ $<

$(B)/MDSYS.COM: mdsys/mdsys.asm | $(B)
	$(NASM) -f bin -o $@ $<

clean:
	rm -rf $(B)

.PHONY: all clean
