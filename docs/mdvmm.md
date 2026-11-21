# MDVMM - modernDOS virtual machine monitor

MDVMM is the 32-bit supervisor in the modernDOS architecture. It runs DOS in
a virtual 8086 machine and will host 32-bit DPMI programs (the future GUI)
and several DOS sessions.

## Status

| Part | State |
|---|---|
| M2.1 V86 monitor | working: DOS, FreeCOM, XMS, disk, timer, keyboard controller under the monitor |
| M2.2 DPMI host | planned |
| M2.3 multiple DOS sessions | planned |

## Using it

```
DEVICE=HIMEMX.EXE        (CONFIG.SYS: any XMS driver)
MDVMM                    (AUTOEXEC.BAT or prompt)
```

MDVMM takes a 16 MB pool from XMS (less if less is free), moves itself there
and keeps under 1 KB of conventional memory. It cannot be unloaded; reboot
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

## Limitations (M2.1)

- Programs that switch to protected mode themselves (without DPMI or VCPI)
  stop the machine with a monitor panic. DPMI arrives in M2.2; VCPI is not
  planned.
- Reprogramming the PIC vector base from DOS is not virtualized.
- A20 is always enabled (programs relying on 1 MB wraparound need LOADFIX).

## Testing

`tests/mdvmm/run.sh` boots HIMEMX + FreeCOM in QEMU, loads MDVMM from
AUTOEXEC.BAT and runs V86CHK, XMSTEST and the long filename suite inside the
virtual machine. Extra QEMU options can be passed in `QEMU_EXTRA`, for
example `-d int,cpu_reset -D qemu.log`.
