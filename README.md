modernDOS
=========

**DOS, reimagined as if it never faded.** modernDOS is a fork of the FreeDOS kernel that imagines DOS kept evolving for 40 years: an original desktop GUI, modern native and networked applications, and booting on modern UEFI PCs. Every existing DOS program keeps working.

### Compatibility contract
INT 21h, INT 2Fh, program loading (EXEC) and the DOS version numbers reported by INT 21h/30h are frozen. New features are added on top of them and never change their existing behavior.

### Roadmap
| # | Milestone | Status |
|---|---|---|
| M1 | Kernel modernization: reproducible container build, QEMU boot tests, native long-filename (LFN) API in the kernel (`docs/lfnapi.txt`) | done |
| M2 | MDVMM supervisor: 32-bit V86 monitor + DPMI host, multiple DOS sessions (`docs/mdvmm.md`) | done |
| M3 | UEFI boot with native drivers (GOP display, AHCI/NVMe disks, USB input) | in progress: DOS boots on UEFI with a virtual BIOS, XMS, DPMI and sessions (M3.1-M3.2) |
| M4 | Original desktop GUI + widget toolkit (32-bit DPMI) | planned |
| M5 | Native apps: file manager, editor, settings, terminal (DOS session in a window) | planned |
| M6 | Networking: TCP/IP, sockets API, package manager, browser | planned |

### Building (Linux, needs podman)
```
scripts/mdbuild.sh      # builds the GCC (reference) and Open Watcom kernels, MDVMM.COM, BOOTX64.EFI
scripts/mdtest.sh       # QEMU: boot tests, LFN API + DJGPP libc, MDVMM (DPMI, sessions), UEFI boot
```

The upstream FreeDOS README follows.

---

fd kernel
=========

FreeDOS kernel - current 0xFD version is 2.45 (2045)

http://www.fdos.org/kernel/

The FreeDOS kernel implements the core MS/PC-DOS (R) compatible functions.  It is derived from Pat Villani's DOS-C kernel and released under the GPL v2.  Please see http://www.freedos.org/ for more details about the FreeDOS (TM) Project.   This was originally a branch based on 2042 SVN 0xFD kernel; the full svn history has since been added to git (see other branches) and current development of release kernels are available here.  Please fork and submit a Pull Request!

Compiled kernels ready to use (just copy kernel.sys over an existing install) are available at http://www.fdos.org/kernel/testing/git/ - also available are the source archive and fdpkg compatible packages.  *** TODO update links/upload archives


This version of the kernel is intended only for 8086+ or 80386+ IBM compatible computers in continuation of the goals of the FreeDOS Project.  Please see http://www.fdos.org/kernel for my 0xDC kernel that is work on merging back in some of the original portability aspects and other supported features at the cost of some compatibility with older hardware/software.

Please feel free to email me - PerditionC@gmail.com

Now with automatic builds and soon tests [![Build](../../workflows/Build/badge.svg)](../../actions)
