# MDVMM - modernDOS virtual machine monitor

MDVMM is the 32-bit supervisor in the modernDOS architecture. It runs DOS in
a virtual 8086 machine and will host 32-bit DPMI programs (the future GUI)
and several DOS sessions.

## Status

| Part | State |
|---|---|
| M2.1 V86 monitor | working: DOS, FreeCOM, XMS, disk, timer, keyboard controller under the monitor |
| M2.2 DPMI host | working: DPMI 0.9 for 16/32 bit clients; DJGPP programs run without CWSDPMI |
| M2.3 multiple DOS sessions | working: up to 4 preemptively scheduled DOS sessions, Ctrl+Alt+F1..F4 |
| M3.1 UEFI boot | working: BOOTX64.EFI starts the core, which provides a virtual PC BIOS (GOP text console, PS/2 keyboard, timer, RTC, disk image from the ESP) and boots DOS |
| M3.2 UEFI: XMS, sessions | working: extended memory for XMS drivers (DOS=HIGH), DPMI, DOS sessions on UEFI boots |
| M3.3 AHCI | working: SATA disks on AHCI controllers are BIOS hard disks 80h+ (EDD), writes reach the disk |
| M3.4 NVMe | working: NVMe namespaces (512-byte blocks) are BIOS hard disks too; registers above 4 GB via PSE-36 |
| M3.5 USB keyboard | working: keyboards on xHCI root ports (HID boot protocol) arrive as PS/2 scancodes, with typematic repeat |

## Using it

```
DEVICE=HIMEMX.EXE        (CONFIG.SYS: any XMS driver)
MDVMM                    (AUTOEXEC.BAT or prompt)
```

```
MDVMM NEW [command]      start another DOS session running COMSPEC
                         (/C command); Ctrl+Alt+F1..F4 switch the screen
```

On a UEFI PC (no CSM), copy `mdvmm/build/BOOTX64.EFI` to
`\EFI\BOOT\BOOTX64.EFI` on the EFI system partition and a bootable DOS
floppy image to `\MDOS\BOOT.IMG`. The monitor is then already
running when DOS starts; `MDVMM` without arguments (AUTOEXEC.BAT) only
sets up DOS sessions. See "UEFI boot" below.

MDVMM takes a 32 MB pool from XMS (less if less is free), moves itself there
and keeps under 1 KB of conventional memory. DPMI programs allocate from the
pool. It cannot be unloaded; reboot
to leave it. It refuses to start when the CPU is already in protected or V86
mode (EMM386, JemmEx, another MDVMM).

## Design

- `mdvmm/loader/loader.asm` - MDVMM.COM: CPU/XMS checks, pool allocation,
  copies the core into the pool, switches to protected mode, and continues
  in V86 mode at `resumed` before going resident.
- `mdvmm/core/` - freestanding 32-bit C, linked at 0xF0000000. It does not
  depend on DOS; the UEFI loader starts the same core.
  - `entry.asm` - position independent start, page tables, interrupt stubs
  - `cpu.c` - GDT, IDT, TSS with I/O permission bitmap, PIC remapped to 20h
  - `mm.c` - pool allocator, temporary physical mappings
  - `v86.c` - the monitor
  - `dpmi.c` - DPMI host
  - `vm.c` - DOS sessions
  - `dma.c` - ISA DMA virtualization
  - `bios.c`, `rom.asm`, `fbcon.c`, `font.c` - virtual PC BIOS (UEFI boot)
  - `pci.c`, `ahci.c`, `nvme.c` - PCI configuration space, device register
    mappings, AHCI and NVMe disk drivers
  - `xhci.c`, `usbkbd.c` - xHCI host controller, USB keyboards
  - `nobios.c` - stubs: the core in MDVMM.COM (64 KB .COM limit) is linked
    without the virtual BIOS and the drivers; BOOTX64.EFI has the full core
- `mdvmm/uefi/` - BOOTX64.EFI: x64 UEFI application with the core embedded.
- The loader and the core share `struct bootinfo` (`mdvmm/include`).

V86 mode runs with IOPL 0. CLI, STI, PUSHF, POPF, INT, IRET and HLT trap into
the monitor and are emulated against a virtual interrupt flag. Hardware
interrupts always reach the monitor and are reflected into the DOS interrupt
vector table when the virtual IF allows it.

Intercepted services:

| Interface | Handling |
|---|---|
| INT 15h AH=87h | extended memory block move, done by the monitor (XMS drivers use it in V86 mode) |
| INT 15h AH=89h | switch to protected mode: refused (CF=1) |
| ports 60h, 64h, 92h | A20 stays enabled: A20 commands are swallowed, other traffic passes through |
| INT 2Fh AX=DE00h | installation check: AL=FFh, BX=4D44h ("MD"), CX=API version |
| INT 2Fh AX=DE01h | write the ASCIZ string at DS:DX to the monitor log |
| INT 2Fh AX=DE02h | fork the running session (parent: AX=new session, child: AX=0, BX=number) |
| INT 2Fh AX=DE03h | end the running session (not session 0) |
| INT 2Fh AX=DE04h | loader registers the per-session DOS data (internal) |
| INT 2Fh AX=DE05h | set focus: BL=session |
| INT 2Fh AX=DE06h | AX=running session, BX=focused session, CX=number of sessions, DX=1 if sessions are set up |
| INT 2Fh AX=DE07h | UEFI boots: install the virtual BIOS mouse driver as INT 33h; AX=1 if installed |
| ports 00-0F, C0-DF, 81-8F | ISA DMA controllers, virtualized (see below) |

The monitor logs to I/O port E9h (QEMU `-debugcon`, Bochs).

## DPMI host

`core/dpmi.c` implements DPMI 0.9 (16 or 32 bit clients). Every DOS
session can have a client, and a client can start a program that becomes
a client itself (four deep): the first waits in its EXEC call. The host's
state (descriptor table, client record, transitions, host stack) is that
of the running session and is swapped on a session switch; each session's
clients have their own slice of the linear address space.
Clients run at ring 3 with IOPL 3, so CLI/STI act on the real interrupt
flag, as with CWSDPMI.

Mode transitions use breakpoints. On the V86 side these are HLT
instructions in the loader's resident stub (`BP_*` in `bootinfo.h`): the
DPMI entry, returns from real-mode calls, raw mode switches and 16 real-mode
callbacks. On the protected-mode side, a ring-3 host code page full of HLT
(selector 2Bh) serves as return addresses for exception, IRQ and callback
handlers and as the default interrupt and exception vectors. Nested
transitions are kept on a stack of saved frames.

| Group | Functions |
|---|---|
| descriptors | 0000-0003h, 0006-000Ch (LDT of 1024 entries) |
| DOS memory | 0100-0102h |
| interrupts | 0200-0205h; IRQs go to PM handlers in PM and in real mode |
| real mode | 0300-0306h, 16 callbacks, raw switch |
| info | 0400h (0.90, 32 bit), 0500h |
| memory | 0501-0503h (paged from the pool), 0600-0604h, 0702/0703h, 0800/0801h |
| virtual IF | 0900-0902h |

The host real-mode stack is the 2 KB of private data the client allocates
for the host (SI=80h from INT 2Fh AX=1687h). For returns to 16-bit client
stacks the monitor uses the "espfix" technique, so ESP[31:16] does not leak
the monitor's stack address.

A DOS program a client starts (e.g. with `system()`) runs normally: its
INT 21h AH=4Ch ends the client only if DOS's current PSP is the client's.
Interrupts a client reflects to real mode also reach the monitor's own
services (INT 2Fh AH=DEh, INT 15h AH=87h).

Not yet: DPMI 1.0 functions, several clients at once (a DPMI program
starting another one), virtual memory.

### DOS extenders and other DOS programs

`tests/compat` runs programs of the kinds DOS software comes in: a 16-bit
real mode program, the same program under DOS/4GW, PMODE/W, DOS/32A and
CauseWay, and the Open Watcom compiler and linker hosted on DOS (DOS/4GW
programs started by a real mode driver). What they needed from the host:

- `MOV r32, CR0` in V86 mode is answered (protected mode on), other
  control and debug register moves are ignored; an instruction a program
  cannot use here (LIDT, a switch to real mode) ends the program, not the
  machine.
- The I/O privilege level in the flags reads as 3 and cannot be changed,
  as under EMM386 and Windows: that is how programs tell V86 mode from
  real mode (DOS/32A).
- A client's own protected mode handlers come first, also for INT 31h
  (CauseWay adds its services there) and INT 21h AH=4Ch (DOS/4GW cleans
  up in its handler before the program ends); they chain to the host.
- Selector 0040h is the BIOS data area (DOS/4GW reads the timer there).
- Real mode vectors a program leaves pointing into its own memory when it
  ends are put back to what they were before the program set them.

`tests/compat/game.sh` plays the start of a game off the shelf
(Kiloblaster, 1992: real mode, its own keyboard and timer handlers, mode
X) on both editions and compares the pictures.

## DOS sessions

All sessions share one DOS kernel, as in Windows 386 enhanced mode.
`MDVMM NEW` forks the running session in the monitor and the child EXECs
COMSPEC; session 0 is the original DOS.

- Memory below the end of MDVMM's resident block (rounded up to 4K) is
  global: kernel, drivers, TSRs loaded before MDVMM. From there to 640K
  each session has private pages.
- Instance data is swapped on every switch: IVT, BIOS data area and
  500h area, the CDS array and the kernel's instance table from the
  INT 2Fh AX=1605h startup broadcast (the swappable data area with the
  DOS stacks, and the private stacks of the built-in drivers and INT 2Fh).
  Global kernel structures (SFTs, buffers, DPBs) are not instanced.
- The kernel is built with WIN31SUPPORT: INT 21h functions other than
  character I/O (01h-0Ch) run inside an INT 2Ah critical section, and the
  monitor never switches sessions inside one.
- Scheduling: round robin, 2 timer ticks per slice; a session that idles
  (INT 28h, INT 2Fh AX=1680h, HLT) yields at once. Missed timer ticks are
  added to the BIOS tick count when a session runs again.
- Hardware interrupts go to the running session, except the keyboard:
  the monitor takes every scancode from the controller (hotkeys are its
  own) and gives it to the session on the display as a keyboard interrupt
  of its own - the scancode at port 60h, the controller's status saying
  so, one keystroke at a time (not before the handler of the one before
  has returned).
- Text mode video: the focused session owns B8000h, the others write to
  shadow pages.
- DOS boots (the machine's own BIOS), "flat" sessions: memory below 640K
  is copied on a switch, not remapped, so a linear address is the
  physical one (BIOS disk code in system management mode and DMA
  hardware read memory by physical address). A session inside a BIOS
  service (INT 10h-1Ah) is not switched out - a BIOS is written for one
  thread (SeaBIOS: one stack of its own for all services) - and a wait
  for a key (INT 16h AH=00h/10h) is done by the monitor in the BIOS's
  place. A session that is not on the display does not reach the video
  hardware: writes to the VGA ports are dropped, a text mode set is done
  in its own screen memory, a graphics mode set waits for the display.
- One network card for all sessions: see `docs/network.md`.
- INT 15h AH=87h block moves translate addresses below 110000h through
  the running session's page tables (XMS moves into private memory).

### ISA DMA

The 8237 controllers use physical addresses, so the monitor owns them.
It keeps a shadow of each channel and programs the real controller
itself: unchanged for identity mapped memory, translated for physically
contiguous memory, otherwise through a 128 KB bounce buffer below 16 MB.
Device->memory data is copied out of the bounce buffer at every hardware
interrupt before any handler runs, when the channel is masked and when
the status register is read. Reads of the current address are translated
back. DPMI clients (IOPL 3) program the controller directly.

## UEFI boot

A UEFI machine has no PC BIOS, and DOS needs one. `BOOTX64.EFI` reads
`\MDOS\BOOT.IMG` from the boot volume and takes the GOP framebuffer,
a 32 MB pool below 4 GB and one page below 640K. It then exits boot
services, leaves long mode (32-bit protected mode, paging off) and starts
the core like the DOS loader does, with `boot_type = BOOT_UEFI` in
`struct bootinfo` (version 3).

It also allocates up to 64 MB of contiguous memory below 4 GB as DOS
extended memory (`xms_phys`/`xms_size`, bootinfo version 4).

Before anything else the loader writes a hardware report,
`\MDOS\HWINFO.TXT`, to the boot volume if it is writable (a USB
stick, not a CD): firmware vendor and version, SMBIOS model, CPU, GOP
modes and UGA, every PCI device with its BARs and IRQ line, the EFI
configuration tables and the memory map (`uefi/hwinfo.c`). It is how
drivers for a real machine are written without access to it.

The core then sets up a PC (`bios.c`):

- `rom.asm` at F000:0000 (RAM from the pool). BIOS services are HLT
  breakpoints followed by RETF 2: the monitor does the work on the V86
  registers. IRQ0/IRQ1 handlers are real 16-bit code with a breakpoint
  inside, so DOS and TSRs hook and chain all vectors as usual. The ROM
  also holds the DPMI stubs, so the DPMI host works without MDVMM.COM.
- IVT, BIOS data area (conventional memory size from the UEFI memory
  map), PIT at 18.2 Hz, PIC masks.
- INT 10h: text mode 3 on RAM at B8000h. `fbcon.c` draws changed cells
  and the cursor on the framebuffer every timer tick, with the 8x16 font
  scaled up to 2x. Teletype output is mirrored to COM1 when a UART exists.
  VBE 3.0 with one mode (141h): the GOP framebuffer, 32 bpp linear; the
  console stops drawing while it is set (see `docs/gui.md`).
- INT 13h: the disk image as floppy drive 00h (geometry from its BPB);
  writes go to memory only. Hard disks from native drivers are drives
  80h and up (BIOS data area 40:75h): CHS functions with a 255/63
  geometry, and the EDD extensions 41h, 42h, 43h, 48h. Transfers go
  through 64 KB of pool pages; drivers implement `struct disk`. Disk
  resets (AH=00h/0Dh, e.g. from INT 21h AH=0Dh) flush the drive's write
  cache.
- AHCI (`ahci.c`): every AHCI controller on PCI (class 01:06:01), every
  port with a SATA disk. Port takeover, IDENTIFY, READ/WRITE DMA (EXT),
  one command at a time, polled with interrupts off (the machine stops
  during a transfer, as with a PIO BIOS).
- NVMe (`nvme.c`): every NVMe controller (class 01:08:02): reset, admin
  queue, one I/O queue pair of 16 entries, polled. Each active namespace
  with 512-byte blocks becomes a hard disk (4K-block namespaces are
  skipped). Disk order: AHCI ports first, then NVMe namespaces.
- Device registers are mapped uncached at EC000000h. Registers above
  4 GB (64-bit BARs, the usual case for NVMe) get 4 MB pages with PSE-36,
  which puts physical address bits 32-39 in the page directory entry; the
  core keeps 32-bit paging.
- INT 16h / IRQ1: PS/2 keyboard (scancode set 1; machines without a
  keyboard controller, e.g. Macs, read FFh and are left alone), BIOS keyboard buffer,
  INT 15h AH=4Fh hook, Ctrl+Alt+Del reboots.
- USB keyboards (`xhci.c`, `usbkbd.c`), like a BIOS's legacy USB
  support: each xHCI controller (class 0C:03:30) is taken from the
  firmware (legacy support handoff) and reset; devices on root ports are
  addressed and the first HID boot keyboard interface gets eight queued
  interrupt transfers. The event ring is polled on every timer tick and
  INT 16h call. Report changes become scancode set 1 make/break codes,
  which reach the focused session as a virtual IRQ1 with the byte at
  port 60h (`vm_key`), so INT 09h hooks see them as PS/2 keys and the
  session hotkeys work. The last key held repeats after 500 ms, about
  9 per second. Not yet: hubs, hotplug, keyboard LEDs.
- USB mice (HID boot protocol) feed an INT 33h mouse driver
  (`mouse.c`); `MDVMM` installs it after DOS has started (DOS resets
  INT 33h). See `docs/gui.md`.
- Extended memory: DOS sees the loader's region at 1 MB, through INT 15h
  E820h, E801h and 88h, so any XMS driver works (HIMEMX, `DOS=HIGH`).
  The HMA (linear 100000h-10FFFFh) is mapped to its first 64 KB. INT 15h
  AH=87h block moves and DPMI 0800h mappings translate addresses in this
  window (`ext_phys`); moves outside it fail.
- INT 1Ah (ticks, CMOS RTC), INT 11h, 12h, 14h, 15h (C0h, 24h, 4Fh),
  17h, 18h, 19h.

Finally the boot sector of the disk image is started at 0000:7C00, as a
BIOS does, and loads the kernel.

## Limitations

- Programs that switch to protected mode themselves without DPMI (raw or
  VCPI) stop the machine with a monitor panic. VCPI is not planned.
- Reprogramming the PIC vector base from DOS is not virtualized.
- A20 is always enabled (programs relying on 1 MB wraparound need LOADFIX).
- UEFI boot: the boot floppy is a RAM copy of the image (writes are
  lost; hard disks are real); USB keyboards only on root ports (no
  hubs, no hotplug), no USB mice; of the VGA modes text, mode 13h (320x200, 256
  colours) and its unchained relatives ("mode X": four planes, page
  flipping, 320x240) - `core/vga.c`: the monitor is the display card, the
  picture is drawn enlarged on the framebuffer; a DPMI client's access to
  the card's ports is caught with I/O breakpoints; in the planar modes
  A0000h is unmapped and `core/vgaemu.c` runs the instructions that touch
  it. Not the 16 colour planar modes; the framebuffer must be below 4 GB
  and 32 bpp RGB/BGR; no ISA DMA (no bounce buffer below 16 MB).
- Sessions: graphics modes are not virtualized (a session that is not on
  the display waits when it sets one; a DPMI client's mode set through
  DPMI is not caught); DPMI clients are not protected from each other
  (their memory is all mapped); on UEFI boots a DPMI client doing its own
  DMA in a session other than 0 gets wrong addresses.

## Testing

`tests/mdvmm/run.sh` boots HIMEMX + FreeCOM in QEMU, loads MDVMM from
AUTOEXEC.BAT and runs V86CHK, XMSTEST, the long filename suite and two
DJGPP programs inside the virtual machine, then starts a second session
(`MDVMM NEW BG.BAT`) and runs SESSTEST in both: preemption while session 0
never yields, Ctrl+Alt+F2/F1 (sent through the QEMU monitor), the floppy
DMA bounce path and file contents written by both sessions. DJTEST
covers libc file I/O with long names; DPMITEST covers memory, exceptions
as signals, a protected-mode IRQ0 handler, a real-mode callback, DOS
memory and descriptors. There is no CWSDPMI on the test disk. Extra QEMU
options can be passed in `QEMU_EXTRA`, for example `-d int,cpu_reset -D qemu.log`.

`tests/uefi/run.sh` boots OVMF from a FAT ESP holding BOOTX64.EFI and the
DOS image (HIMEMX, `DOS=HIGH`), runs the MDVMM test programs (V86CHK,
XMSTEST, LFNTEST, DJTEST, DPMITEST, a second session with SESSTEST) and
DISKTEST on a 64 MB AHCI disk as C: and a 96 MB NVMe disk as D: (MBR,
FAT16; OVMF puts the NVMe registers above 4 GB). After QEMU exits both
disks are checked from Linux: fsck, a 1 MB pattern file, FROMDOS.TXT. It
also types a command at the FreeCOM prompt through QEMU `sendkey` and
checks the echo in the COM1 mirror of the screen; a key held for 1.5 s
must repeat. All keys go to a USB keyboard on an xHCI controller.
`SCREEN=file.ppm` saves the framebuffer.
