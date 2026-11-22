# MDVMM - modernDOS virtual machine monitor
# Builds build/MDVMM.COM (16-bit DOS loader with the 32-bit core embedded).

CC      = i686-linux-gnu-gcc
LD      = i686-linux-gnu-ld
OBJCOPY = i686-linux-gnu-objcopy
NASM    = nasm

CFLAGS  = -m32 -std=gnu11 -O2 -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
          -fno-delete-null-pointer-checks -fno-strict-aliasing -fno-asynchronous-unwind-tables \
          -mgeneral-regs-only --param=min-pagesize=0 -Iinclude -Icore
NFLAGS  = -Iinclude/

B       = build
COBJS   = $(B)/entry.o $(B)/main.o $(B)/lib.o $(B)/mm.o $(B)/cpu.o $(B)/v86.o $(B)/dpmi.o $(B)/vm.o $(B)/dma.o
HDRS    = core/core.h include/bootinfo.h

all: $(B)/MDVMM.COM

$(B):
	mkdir -p $(B)

$(B)/entry.o: core/entry.asm include/bootinfo.inc | $(B)
	$(NASM) $(NFLAGS) -f elf32 -o $@ $<

$(B)/%.o: core/%.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/core.elf: $(COBJS) core/core.ld
	$(LD) -m elf_i386 -z noexecstack --no-warn-rwx-segments -T core/core.ld -o $@ $(COBJS)

$(B)/core.bin: $(B)/core.elf
	$(OBJCOPY) -O binary $< $@

$(B)/MDVMM.COM: loader/loader.asm $(B)/core.bin include/bootinfo.inc
	$(NASM) $(NFLAGS) -I$(B)/ -f bin -o $@ $<

clean:
	rm -rf $(B)

.PHONY: all clean
