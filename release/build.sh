#!/bin/bash
# Build the modernDOS release ISO (runs inside the build container, after
# scripts/mdbuild.sh). Output: _output/release/modernDOS-$VERSION.iso
#
# The ISO boots three ways:
#   - UEFI, from CD or USB stick (dd): an El Torito EFI image that is also
#     an appended GPT partition (EFI system partition) holding
#     \EFI\BOOT\BOOTX64.EFI and \MDOS\BOOT.IMG
#   - BIOS, from CD: El Torito floppy emulation of BOOT.IMG
# BOOT.IMG is a DOS disk image: kernel, FreeCOM, HIMEMX, MDDESK, CTMOUSE (BIOS
# boots; on UEFI boots MDVMM installs the virtual BIOS mouse driver), MDVMM,
# demo, and the network programs: PKTDRV (packet driver on MDVMM's network
# card drivers), SSHSLOT, SSHD, MDLOG, MDSYS, NET.BAT. The one a BIOS starts
# from CD is a 2.88 MB floppy; the one for UEFI boots (on the EFI system
# partition) has 8 MB and also holds the SSH client and the package manager.
#
# A personal image for one's own machine: PERSONAL=<dir> adds the files of
# that directory to BOOT.IMG, typically AUTHKEYS.TXT (public keys allowed
# to log in) and HOSTKEY.ED (32 random bytes: the SSH host key, so it stays
# the same over reboots; A: is a RAM disk). With a file AUTONET on the disk
# AUTOEXEC.BAT starts the network and the SSH server (NET.BAT). Never
# publish such an image: it contains the host's private key.
#
# On UEFI boots the loader writes \MDOS\HWINFO.TXT (hardware report)
# and \MDOS\BOOTLOG.TXT (its own log) to the stick. A version ending
# in "-dev" is a diagnostic build: it also has \MDOS\BOOTWAIT, which
# keeps the monitor's boot log on the screen for 15 seconds.
#
# Usage: release/build.sh [version]

set -e
VERSION=${1:-0.0.1-alpha}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HERE="$ROOT/release"
TEST="$ROOT/test"
KERNEL="$ROOT/_output/gcc/kgc8632.sys"
OUT="$ROOT/_output/release"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
. "$TEST/cfg.sh"
export MTOOLS_SKIP_CHECK=1

[ -f "$KERNEL" ] || { echo "no $KERNEL: run scripts/mdbuild.sh first"; exit 1; }
make -s -C "$ROOT/mdvmm"
make -s -C "$ROOT/gui"
make -s -C "$ROOT/net/ssh"
make -s -C "$ROOT/net/pkg"
/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc -O2 -s -I"$ROOT/gui/lib" -o "$WORK/gfxdemo.exe" \
  "$ROOT/tests/gui/gfxtest.c" "$ROOT/gui/build/mdgui.o"

echo "== BOOT.IMG"
cd "$WORK"
printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\r\n' > fdconfig.sys
cat > autoexec.bat <<EOF
@ECHO OFF
PROMPT \$P\$G
PATH A:\\
ECHO.
ECHO modernDOS $VERSION - see A:\\README.TXT
ECHO   MDDESK       the desktop
ECHO   MDVMM NEW    another DOS session (Ctrl+Alt+F1..F4 switch)
ECHO   GFXDEMO      graphics test picture (3 seconds)
ECHO.
ECHO   NET          the SSH server (needs AUTHKEYS.TXT or SSHD /PASSWD)
ECHO   PKG          package manager; SSH: the SSH client (UEFI boots)
ECHO   MDLOG        the monitor's log; MDLOG /N with the network card's state
ECHO.
ECHO   MDSYS        state; MDSYS REBOOT restarts; TRYBOOT / KEEP: try a new build
ECHO.
SET WATTCP.CFG=A:\\
SET TEMP=A:\\
A:\\CTMOUSE.EXE >NUL
A:\\MDVMM.COM
REM DOS sessions for the desktop's terminal and for remote shells, then the
REM packet driver (this order: the sessions must not carry a copy of it)
A:\\SSHSLOT.COM 2 >NUL
A:\\PKTDRV.COM >NUL
REM installed on a disk: C:\\MDOS keeps the SSH keys and settings
REM (not on a start from a stick: the machine's disks are read-only then)
A:\\MDSYS.COM DISK
IF ERRORLEVEL 1 GOTO NODISK
IF NOT EXIST C:\\MDOS\\BOOT.IMG GOTO NODISK
IF EXIST C:\\MDOS\\SSH\\HOSTKEY.ED GOTO HAVESSH
MD C:\\MDOS\\SSH
IF EXIST A:\\AUTHKEYS.TXT COPY A:\\AUTHKEYS.TXT C:\\MDOS\\SSH >NUL
IF EXIST A:\\HOSTKEY.ED COPY A:\\HOSTKEY.ED C:\\MDOS\\SSH >NUL
COPY A:\\WATTCP.CFG C:\\MDOS\\SSH >NUL
:HAVESSH
SET SSHDIR=C:\\MDOS\\SSH
SET WATTCP.CFG=C:\\MDOS\\SSH
SET TEMP=C:\\MDOS
SET PKGDIR=C:\\MDOS
IF EXIST C:\\MDOS\\SSH\\AUTHKEYS.TXT CALL A:\\NET.BAT
GOTO END
:NODISK
IF EXIST A:\\AUTONET CALL A:\\NET.BAT
IF EXIST A:\\AUTONET GOTO END
REM a personal stick: its keys are on A:
IF EXIST A:\\AUTHKEYS.TXT CALL A:\\NET.BAT
:END
EOF
cat > tryboot.bat <<EOF
@ECHO OFF
REM start the build in C:\\MDOS\\TRY once; without MDSYS KEEP (or KEEP)
REM within ten minutes the machine restarts into the installed build
IF NOT EXIST C:\\MDOS\\TRY\\BOOTX64.EFI GOTO MISSING
IF NOT EXIST C:\\MDOS\\TRY\\BOOT.IMG GOTO MISSING
ECHO try once > C:\\MDOS\\TRYONCE
A:\\MDSYS.COM REBOOT
:MISSING
ECHO Put BOOTX64.EFI and BOOT.IMG into C:\\MDOS\\TRY first.
EOF
cat > keep.bat <<EOF
@ECHO OFF
REM keep the build that runs as a trial: confirm it, then make it the
REM installed one
A:\\MDSYS.COM KEEP
IF NOT ERRORLEVEL 2 GOTO NOTRIAL
COPY /Y C:\\MDOS\\TRY\\BOOT.IMG C:\\MDOS\\BOOT.IMG
COPY /Y C:\\MDOS\\TRY\\BOOTX64.EFI C:\\EFI\\BOOT\\BOOTX64.EFI
ECHO The trial build is the installed one now.
GOTO END
:NOTRIAL
ECHO This is not a trial boot: nothing to keep.
:END
EOF
cat > net.bat <<EOF
@ECHO OFF
REM the SSH server, with a local DOS prompt beside it (session 3,
REM Ctrl+Alt+F4). AUTOEXEC.BAT started the job slots for remote shells
REM (sessions 1 and 2) and the packet driver.
MDVMM.COM NEW
ECHO SSH server starting. Esc stops it. Ctrl+Alt+F4: a DOS prompt, Ctrl+Alt+F1: back.
SSHD.EXE
EOF
printf 'my_ip = dhcp\r\n' > wattcp.cfg
sed -i 's/$/\r/' autoexec.bat net.bat tryboot.bat keep.bat
sed 's/$/\r/' "$HERE/README.TXT" > readme.txt

# make_image file sectors per-cluster FAT-sectors sectors-per-track: a FAT12
# boot disk image with the kernel
make_image() {
  "$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
    -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
    -D_FAT12 -D_UNIT=00h -D_CHS_SECTORS=$5 -D_CHS_HEADS=2 -o boot.bin
  "$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
    -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
    -D_BOOTPATCHFILE="'boot.bin'" \
    -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
    -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=$3 -D_SPI=$2 \
    -D_SPF=$4 -D_NUMROOT=224 -D_CHS_SECTORS=$5 -D_CHS_HEADS=2 -D_UNIT=00h -o "$1"
}
# put name=file ... image: files onto an image
put() {
  local img=$1 f; shift
  for f in "$@"; do mcopy -i "$img" "${f#*=}" "::${f%%=*}"; done
}
BASE="COMMAND.COM=/opt/fdos/bin/command.com HIMEMX.EXE=/opt/fdos/bin/himemx.exe
  AUTOEXEC.BAT=autoexec.bat README.TXT=readme.txt MDVMM.COM=$ROOT/mdvmm/build/MDVMM.COM
  GFXDEMO.EXE=gfxdemo.exe MDDESK.EXE=$ROOT/gui/build/MDDESK.EXE CTMOUSE.EXE=/opt/fdos/bin/ctmouse.exe
  PKTDRV.COM=$ROOT/mdvmm/build/PKTDRV.COM SSHSLOT.COM=$ROOT/mdvmm/build/SSHSLOT.COM
  MDLOG.COM=$ROOT/mdvmm/build/MDLOG.COM MDSYS.COM=$ROOT/mdvmm/build/MDSYS.COM
  SSHD.EXE=$ROOT/net/ssh/build/sshd.exe NET.BAT=net.bat TRYBOOT.BAT=tryboot.bat KEEP.BAT=keep.bat
  WATTCP.CFG=wattcp.cfg"
# more for UEFI boots, where the disk image can be any size
MORE="SSH.EXE=$ROOT/net/ssh/build/ssh.exe PKG.EXE=$ROOT/net/pkg/build/pkg.exe"

# boot.img: 2.88 MB, what a BIOS can start from CD (El Torito floppy
# emulation); uefi.img: 8 MB (130 cylinders, 2 heads, 63 sectors)
make_image boot.img 5760 2 9 36
put boot.img $BASE
make_image uefi.img 16380 8 6 63
put uefi.img $BASE $MORE
if [ -n "$PERSONAL" ]; then
  for f in "$PERSONAL"/*; do
    [ -f "$f" ] || continue
    for img in boot.img uefi.img; do
      mcopy -i $img "$f" "::$(basename "$f" | tr a-z A-Z)"
    done
  done
  echo "personal image: files of $PERSONAL added - do not publish it"
fi
mdir -i uefi.img ::

# The BIOS edition: a hard disk image (MBR, one active 64 MB FAT16
# partition) for a USB stick or a disk, started by a PC BIOS. The system is
# in C:\MDOS; C: is the stick or disk itself, so everything is kept.
echo "== BIOS disk image"
mkdir -p hd
printf 'DEVICE=C:\\MDOS\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=C:\\COMMAND.COM C:\\ /E:1024 /P\r\n' > hd/fdconfig.sys
cat > hd/autoexec.bat <<EOF
@ECHO OFF
PROMPT \$P\$G
PATH C:\\MDOS;C:\\
ECHO.
ECHO modernDOS $VERSION (BIOS edition) - see C:\\MDOS\\README.TXT
ECHO   MDDESK       the desktop
ECHO   MDVMM NEW    another DOS session (Ctrl+Alt+F1..F4 switch)
ECHO   NET          the SSH server (needs AUTHKEYS.TXT or SSHD /PASSWD)
ECHO   PKG          package manager; SSH: the SSH client
ECHO   MDLOG        the monitor's log; MDLOG /N with the network card's state
ECHO.
SET SSHDIR=C:\\MDOS\\SSH
SET WATTCP.CFG=C:\\MDOS\\SSH
SET TEMP=C:\\MDOS\\TEMP
SET PKGDIR=C:\\MDOS
CTMOUSE.EXE >NUL
MDVMM.COM
REM DOS sessions for remote shells, then the packet driver (this order:
REM the sessions must not carry a copy of it)
SSHSLOT.COM 2 >NUL
PKTDRV.COM >NUL
IF EXIST C:\\MDOS\\SSH\\AUTHKEYS.TXT CALL C:\\MDOS\\NET.BAT
EOF
sed -i 's/$/\r/' hd/autoexec.bat
"$NASM" -f bin -o mbr.bin "$HERE/mbr.asm"
# CHS only: the partition is small and near the start of the disk, and
# old BIOSes have nothing else
"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_FAT16=1 -D_UNIT=80h -D_LBA=0 -D_RPL=0 -o boot16.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,hd/fdconfig.sys,hd/autoexec.bat" \
  -D_BOOTPATCHFILE="'boot16.bin'" -D_MBRPATCHFILE="'mbr.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR -D_MBR_PART_TYPE=ptFAT16 -D_BPE=16 -D_SPC=4 -D_SPI=131072 -D_SPF=128 \
  -D_NUMROOT=512 -D_CHS_SECTORS=63 -D_CHS_HEADS=16 -D_UNIT=80h -o bios.img
HD="bios.img@@$((1008 * 512))"          # the partition (after one cylinder)
mcopy -i "$HD" /opt/fdos/bin/command.com ::COMMAND.COM
mmd -i "$HD" ::/MDOS ::/MDOS/SSH ::/MDOS/TEMP
for f in $BASE $MORE; do
  case "${f%%=*}" in
    COMMAND.COM|AUTOEXEC.BAT|TRYBOOT.BAT|KEEP.BAT|WATTCP.CFG) ;;   # not here / UEFI only
    *) mcopy -i "$HD" "${f#*=}" "::/MDOS/${f%%=*}" ;;
  esac
done
mcopy -i "$HD" wattcp.cfg ::/MDOS/SSH/WATTCP.CFG
if [ -n "$PERSONAL" ]; then
  for f in "$PERSONAL"/*; do
    [ -f "$f" ] && mcopy -i "$HD" "$f" "::/MDOS/SSH/$(basename "$f" | tr a-z A-Z)"
  done
fi
mdir -i "$HD" ::/MDOS | tail -3

echo "== EFI system partition image"
# 30 MB, FAT16: an El Torito EFI image must stay below 32 MB (16 bit sector
# count), which rules out FAT32; room for the report files and more
dd if=/dev/zero of=efiboot.img bs=1M count=30 status=none
mformat -i efiboot.img -v MODERNDOS ::
mmd -i efiboot.img ::/EFI ::/EFI/BOOT ::/MDOS
mcopy -i efiboot.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i efiboot.img uefi.img ::/MDOS/BOOT.IMG
mcopy -i efiboot.img readme.txt ::/README.TXT
if [ -n "$PERSONAL" ]; then
  # a personal stick offers to install itself on the machine's disk
  printf 'The loader offers to install modernDOS on the internal disk.\r\n' > install
  mcopy -i efiboot.img install ::/MDOS/INSTALL
fi
case "$VERSION" in
  *-dev*)
    printf 'Keeps the boot log on the screen for 15 seconds. Delete this file to boot faster.\r\n' > bootwait
    mcopy -i efiboot.img bootwait ::/MDOS/BOOTWAIT
    echo "diagnostic build: \\MDOS\\BOOTWAIT added" ;;
esac
minfo -i efiboot.img :: | grep -i "disk type"

echo "== ISO"
mkdir -p iso/MDOS iso/EFI/BOOT
cp boot.img iso/MDOS/BOOT.IMG
cp "$ROOT/mdvmm/build/BOOTX64.EFI" iso/EFI/BOOT/BOOTX64.EFI
cp readme.txt iso/README.TXT
sed 's/$/\r/' "$ROOT/COPYING" > iso/COPYING.TXT
mkdir -p "$OUT"
ISO="$OUT/modernDOS-$VERSION.iso"
xorriso -as mkisofs -o "$ISO" -V "MODERNDOS" -R -J \
  -b MDOS/BOOT.IMG -c MDOS/BOOT.CAT \
  -eltorito-alt-boot -e --interval:appended_partition_2:all:: -no-emul-boot \
  -append_partition 2 0xef efiboot.img -appended_part_as_gpt \
  iso 2>&1 | grep -v "^xorriso : UPDATE" || true
[ -f "$ISO" ] || { echo "xorriso failed"; exit 1; }
cp bios.img "$OUT/modernDOS-$VERSION-bios.img"
(cd "$OUT" && sha256sum "$(basename "$ISO")" > "$(basename "$ISO").sha256" &&
  sha256sum "modernDOS-$VERSION-bios.img" > "modernDOS-$VERSION-bios.img.sha256")
ls -l "$OUT"
