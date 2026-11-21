# MDVMM - modernDOS virtual machine monitor

MDVMM is the 32-bit supervisor in the modernDOS architecture. It runs DOS in
a virtual 8086 machine and will host 32-bit DPMI programs (the future GUI)
and several DOS sessions.

## Status

| Part | State |
|---|---|
| M2.1 V86 monitor | working: DOS, FreeCOM, XMS, disk, timer, keyboard controller under the monitor |
| M2.2 DPMI host | working: DPMI 0.9 for 16/32 bit clients; DJGPP programs run without CWSDPMI |
| M2.3 multiple DOS sessions | planned |

## Using it

```
DEVICE=HIMEMX.EXE        (CONFIG.SYS: any XMS driver)
MDVMM                    (AUTOEXEC.BAT or prompt)
```

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
  depend on DOS; a UEFI loader (M3) will start the same core.
  - `entry.asm` - position independent start, page tables, interrupt stubs
  - `cpu.c` - GDT, IDT, TSS with I/O permission bitmap, PIC remapped to 20h
  - `mm.c` - pool allocator, temporary physical mappings
  - `v86.c` - the monitor
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

The monitor logs to I/O port E9h (QEMU `-debugcon`, Bochs).

## DPMI host

`core/dpmi.c` implements DPMI 0.9 for one client at a time (16 or 32 bit).
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

Not yet: DPMI 1.0 functions, several clients at once (a DPMI program
starting another one), virtual memory.

## Limitations

- Programs that switch to protected mode themselves without DPMI (raw or
  VCPI) stop the machine with a monitor panic. VCPI is not planned.
- Reprogramming the PIC vector base from DOS is not virtualized.
- A20 is always enabled (programs relying on 1 MB wraparound need LOADFIX).

## Testing

`tests/mdvmm/run.sh` boots HIMEMX + FreeCOM in QEMU, loads MDVMM from
AUTOEXEC.BAT and runs V86CHK, XMSTEST, the long filename suite and two
DJGPP programs inside the virtual machine. DJTEST covers libc file I/O with
long names; DPMITEST covers memory, exceptions as signals, a protected-mode
IRQ0 handler, a real-mode callback, DOS memory and descriptors. There is no
CWSDPMI on the test disk. Extra QEMU options can be passed in `QEMU_EXTRA`, for
example `-d int,cpu_reset -D qemu.log`.
