modernDOS (alpha)
=================

DOS, reimagined as if it never faded. A fork of the FreeDOS kernel with
a 32-bit supervisor (MDVMM): DOS runs in a virtual 8086 machine, with a
DPMI host, several DOS sessions, and boot on modern UEFI PCs.

THIS IS AN ALPHA. It has been tested in QEMU (OVMF and SeaBIOS) only.
Expect it to fail on some real machines.

Booting
-------
Write the ISO to a USB stick with a raw image writer (dd, balenaEtcher,
Rufus in "DD image" mode), or burn it to a CD.

  UEFI PCs:     boot the stick in UEFI mode. Secure Boot must be OFF
                (BOOTX64.EFI is not signed). No CSM needed.
  BIOS PCs:     boot from CD (El Torito floppy emulation). Booting the
                USB stick needs UEFI.

On UEFI, MDVMM starts first and provides the PC BIOS that DOS expects:
text console on the firmware's framebuffer (GOP, or UGA on older Apple
firmware), keyboard (PS/2 or USB on xHCI root ports), USB mouse (INT 33h), timer, SATA (AHCI) and NVMe disks, VBE
graphics, extended memory for XMS. DOS then boots from \MODERNDOS\BOOT.IMG (drive A:, a RAM
copy: changes to A: are lost at reboot).

Each UEFI boot writes two files to the stick, in \MODERNDOS:
  HWINFO.TXT    hardware report (firmware, model, CPU, graphics, PCI)
  BOOTLOG.TXT   what the loader did, up to the start of the monitor
The monitor then shows its own log on the screen until DOS starts; a file
\MODERNDOS\BOOTWAIT keeps that log visible for 15 seconds. If a machine
does not boot, these two files and a photo of the screen say why.

Hard disks are READ-ONLY by default. DOS can see FAT partitions on the
internal disks, including the EFI system partition of an installed OS.
To allow writes, create a file \MODERNDOS\DISKRW on the boot medium -
at your own risk.

At the DOS prompt
-----------------
  MDDESK         the desktop: launcher (About, Files, Editor,
                 Settings, Run a DOS program, Exit), window list, clock
  MDVMM          on UEFI: mouse driver and DOS sessions; on BIOS PCs:
                 start the virtual machine monitor (needs HIMEMX)
  MDVMM NEW      start another DOS session; Ctrl+Alt+F1..F4 switch
  GFXDEMO        draw a test picture in graphics mode for 3 seconds

Not yet: starting 32-bit DPMI programs from the desktop, USB keyboards on
UHCI/EHCI controllers or behind hubs, network cards other than Intel
e1000 (network and SSH programs are in the source tree, not on this disk).

Source: https://github.com/kaleaditya28897-linux/modernDOS (branch modern)
License: GNU GPL v2 (see COPYING). FreeCOM and HIMEMX are from FreeDOS.
