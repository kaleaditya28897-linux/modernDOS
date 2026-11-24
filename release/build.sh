#!/bin/bash
# Build the modernDOS release ISO (runs inside the build container, after
# scripts/mdbuild.sh). Output: _output/release/modernDOS-$VERSION.iso
#
# The ISO boots three ways:
#   - UEFI, from CD or USB stick (dd): an El Torito EFI image that is also
#     an appended GPT partition (EFI system partition) holding
#     \EFI\BOOT\BOOTX64.EFI and \MODERNDOS\BOOT.IMG
#   - BIOS, from CD: El Torito floppy emulation of BOOT.IMG
# BOOT.IMG is a 1.44 MB DOS floppy: kernel, FreeCOM, HIMEMX, MDDESK, CTMOUSE (BIOS
# boots; on UEFI boots MDVMM installs the virtual BIOS mouse driver), MDVMM,
# demo.
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
A:\\CTMOUSE.EXE >NUL
A:\\MDVMM.COM
EOF
sed -i 's/$/\r/' autoexec.bat
sed 's/$/\r/' "$HERE/README.TXT" > readme.txt

"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=1 -D_SPI=2880 \
  -D_SPF=9 -D_NUMROOT=224 -D_UNIT=00h -o boot.img
mcopy -i boot.img /opt/fdos/bin/command.com ::COMMAND.COM
mcopy -i boot.img /opt/fdos/bin/himemx.exe ::HIMEMX.EXE
mcopy -i boot.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i boot.img readme.txt ::README.TXT
mcopy -i boot.img "$ROOT/mdvmm/build/MDVMM.COM" ::MDVMM.COM
mcopy -i boot.img gfxdemo.exe ::GFXDEMO.EXE
mcopy -i boot.img "$ROOT/gui/build/MDDESK.EXE" ::MDDESK.EXE
mcopy -i boot.img /opt/fdos/bin/ctmouse.exe ::CTMOUSE.EXE
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
