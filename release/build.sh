#!/bin/bash
# Build the modernDOS release ISO (runs inside the build container, after
# scripts/mdbuild.sh). Output: _output/release/modernDOS-$VERSION.iso
#
# The ISO boots three ways:
#   - UEFI, from CD or USB stick (dd): an El Torito EFI image that is also
#     an appended GPT partition (EFI system partition) holding
#     \EFI\BOOT\BOOTX64.EFI and \MODERNDOS\BOOT.IMG
#   - BIOS, from CD: El Torito floppy emulation of BOOT.IMG
# BOOT.IMG is a 2.88 MB DOS floppy: kernel, FreeCOM, HIMEMX, MDDESK, CTMOUSE (BIOS
# boots; on UEFI boots MDVMM installs the virtual BIOS mouse driver), MDVMM,
# demo, and the network programs: PKTDRV (packet driver on MDVMM's network
# card drivers), SSHSLOT, SSHD, SSH, MDLOG, NET.BAT.
#
# A personal image for one's own machine: PERSONAL=<dir> adds the files of
# that directory to BOOT.IMG, typically AUTHKEYS.TXT (public keys allowed
# to log in) and HOSTKEY.ED (32 random bytes: the SSH host key, so it stays
# the same over reboots; A: is a RAM disk). With AUTHKEYS.TXT on the disk
# AUTOEXEC.BAT starts the network and the SSH server. Never publish such
# an image: it contains the host's private key.
#
# On UEFI boots the loader writes \MODERNDOS\HWINFO.TXT (hardware report)
# and \MODERNDOS\BOOTLOG.TXT (its own log) to the stick. A version ending
# in "-dev" is a diagnostic build: it also has \MODERNDOS\BOOTWAIT, which
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
ECHO   NET          network and SSH server (needs A:\\AUTHKEYS.TXT or SSHD /PASSWD)
ECHO   MDLOG        the monitor's log; MDLOG /N with the network card's state
ECHO.
SET WATTCP.CFG=A:\\
SET TEMP=A:\\
A:\\CTMOUSE.EXE >NUL
A:\\MDVMM.COM
IF EXIST A:\\AUTHKEYS.TXT CALL A:\\NET.BAT
EOF
cat > net.bat <<EOF
@ECHO OFF
REM network and SSH server: job slots for remote shells (sessions 1 and 2),
REM a local DOS prompt (session 3, Ctrl+Alt+F4), packet driver, SSHD
A:\\SSHSLOT.COM 2
A:\\MDVMM.COM NEW
A:\\PKTDRV.COM
IF ERRORLEVEL 1 GOTO NOCARD
ECHO SSH server starting. Esc stops it. Ctrl+Alt+F4: a DOS prompt, Ctrl+Alt+F1: back.
A:\\SSHD.EXE
GOTO END
:NOCARD
ECHO No network card: MDLOG shows what the monitor found.
:END
EOF
printf 'my_ip = dhcp\r\n' > wattcp.cfg
sed -i 's/$/\r/' autoexec.bat net.bat
sed 's/$/\r/' "$HERE/README.TXT" > readme.txt

"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=2 -D_SPI=5760 \
  -D_SPF=9 -D_NUMROOT=240 -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -D_UNIT=00h -o boot.img
mcopy -i boot.img /opt/fdos/bin/command.com ::COMMAND.COM
mcopy -i boot.img /opt/fdos/bin/himemx.exe ::HIMEMX.EXE
mcopy -i boot.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i boot.img readme.txt ::README.TXT
mcopy -i boot.img "$ROOT/mdvmm/build/MDVMM.COM" ::MDVMM.COM
mcopy -i boot.img gfxdemo.exe ::GFXDEMO.EXE
mcopy -i boot.img "$ROOT/gui/build/MDDESK.EXE" ::MDDESK.EXE
mcopy -i boot.img /opt/fdos/bin/ctmouse.exe ::CTMOUSE.EXE
mcopy -i boot.img "$ROOT/mdvmm/build/PKTDRV.COM" ::PKTDRV.COM
mcopy -i boot.img "$ROOT/mdvmm/build/SSHSLOT.COM" ::SSHSLOT.COM
mcopy -i boot.img "$ROOT/mdvmm/build/MDLOG.COM" ::MDLOG.COM
mcopy -i boot.img "$ROOT/net/ssh/build/sshd.exe" ::SSHD.EXE
mcopy -i boot.img "$ROOT/net/ssh/build/ssh.exe" ::SSH.EXE
mcopy -i boot.img net.bat ::NET.BAT
mcopy -i boot.img wattcp.cfg ::WATTCP.CFG
if [ -n "$PERSONAL" ]; then
  for f in "$PERSONAL"/*; do
    [ -f "$f" ] || continue
    mcopy -i boot.img "$f" "::$(basename "$f" | tr a-z A-Z)"
  done
  echo "personal image: files of $PERSONAL added - do not publish it"
fi
mdir -i boot.img ::

echo "== EFI system partition image"
# 30 MB, FAT16: an El Torito EFI image must stay below 32 MB (16 bit sector
# count), which rules out FAT32; room for the report files and more
dd if=/dev/zero of=efiboot.img bs=1M count=30 status=none
mformat -i efiboot.img -v MODERNDOS ::
mmd -i efiboot.img ::/EFI ::/EFI/BOOT ::/MODERNDOS
mcopy -i efiboot.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i efiboot.img boot.img ::/MODERNDOS/BOOT.IMG
mcopy -i efiboot.img readme.txt ::/README.TXT
case "$VERSION" in
  *-dev*)
    printf 'Keeps the boot log on the screen for 15 seconds. Delete this file to boot faster.\r\n' > bootwait
    mcopy -i efiboot.img bootwait ::/MODERNDOS/BOOTWAIT
    echo "diagnostic build: \\MODERNDOS\\BOOTWAIT added" ;;
esac
minfo -i efiboot.img :: | grep -i "disk type"

echo "== ISO"
mkdir -p iso/MODERNDOS iso/EFI/BOOT
cp boot.img iso/MODERNDOS/BOOT.IMG
cp "$ROOT/mdvmm/build/BOOTX64.EFI" iso/EFI/BOOT/BOOTX64.EFI
cp readme.txt iso/README.TXT
sed 's/$/\r/' "$ROOT/COPYING" > iso/COPYING.TXT
mkdir -p "$OUT"
ISO="$OUT/modernDOS-$VERSION.iso"
xorriso -as mkisofs -o "$ISO" -V "MODERNDOS" -R -J \
  -b MODERNDOS/BOOT.IMG -c MODERNDOS/BOOT.CAT \
  -eltorito-alt-boot -e --interval:appended_partition_2:all:: -no-emul-boot \
  -append_partition 2 0xef efiboot.img -appended_part_as_gpt \
  iso 2>&1 | grep -v "^xorriso : UPDATE" || true
[ -f "$ISO" ] || { echo "xorriso failed"; exit 1; }
(cd "$OUT" && sha256sum "$(basename "$ISO")" > "$(basename "$ISO").sha256")
ls -l "$OUT"
