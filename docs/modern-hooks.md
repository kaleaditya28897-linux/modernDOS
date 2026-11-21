# Kernel hooks for the MDVMM supervisor

MDVMM (milestone M2) is a planned 32-bit supervisor that runs the modernDOS kernel and legacy programs in V86 virtual machines. It will cooperate with the kernel the way Windows 3.x/9x enhanced mode cooperated with MS-DOS. That interface already exists in the FreeDOS kernel, and modernDOS treats it as a stable contract.

## Interfaces the kernel provides

| Call | Where | Status | Purpose |
|---|---|---|---|
| INT 2Fh AX=1605h | `kernel/inthndlr.c` (AH=16h) | implemented | Startup broadcast. The kernel returns ES:BX pointing to a startup structure with its instance data table (`kernel/kernel.asm`, `instance_table`, ending at `_markEndInstanceData`). These are the DOS data areas the supervisor must keep per VM. Sets `winInstanced`. |
| INT 2Fh AX=1606h | same | implemented | Exit broadcast. Clears `winInstanced`. |
| INT 2Fh AX=1607h, BX=0015h | same | implemented | DOSMGR device API: query support, return the kernel patch table, enable/disable compatibility behaviour. |
| INT 2Fh AX=1680h | `kernel/int2f.asm` | implemented | Idle / release time slice. The kernel calls it from its idle loop and may HLT, depending on `IDLEHALT`. A supervisor hooks it to schedule other VMs. |
| INT 2Ah AH=80h/81h | `kernel/entry.asm` | **built only with `WIN31SUPPORT`** | Enter or leave a DOS critical section around INT 21h, signalled only while `winInstanced` is set. |
| INT 2Ah AH=82h | `kernel/entry.asm` | implemented | End of all critical sections (server hook), called on INT 21h exit. |
| InDOS flag / SDA | `kernel/entry.asm`, INT 21h AX=5D06h | implemented | Lets a supervisor tell whether DOS can be re-entered safely. |

## Decisions for M2

- **Turn on `WIN31SUPPORT` in the GCC and Watcom builds**, so the kernel signals critical sections once a supervisor has announced itself through 1605h. The code path is inert until then (`winInstanced == 0`). It stays off in milestone 1 because nothing can test it yet.
- MDVMM should announce itself with a Windows-compatible 1605h broadcast. DOS TSRs and drivers that already understand Windows (SHARE, network clients, mouse drivers) then work unchanged.
- The kernel keeps all hardware access behind BIOS interrupts (INT 10h/13h/16h/1Ah). This is the boundary where MDVMM substitutes native drivers on UEFI machines.

## Long filename API

INT 21h AH=71h is implemented natively in `kernel/lfnapi.c`. It answers local FAT drives only. Network and redirected drives (CD-ROM, network clients) get AX=7100h with CF set, so the redirector or a TSR can handle them. A supervisor needs no special handling for it.
