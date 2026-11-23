#!/bin/bash
# Generate Watt-32's errno tables for DJGPP (runs inside the build container).
#
# Watt-32 needs build/djgpp/syserr.c and inc/sys/djgpp.err, made by its
# util/errnos.c compiled with DJGPP and run under DOS (it asks DJGPP's C
# library for its errno values and messages). This builds it, runs it in
# QEMU on the modernDOS kernel with CWSDPMI, and writes the two files to
# build/watt32/ (they are kept in the repository; rerun when DJGPP or the
# pinned Watt-32 changes).
#
# Usage: build/watt32-errnos.sh <watt32 source dir>

set -e
WATT=$(realpath "$1")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/watt32"
KERNEL="$ROOT/_output/gcc/kgc8632.sys"
TEST="$ROOT/test"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
. "$TEST/cfg.sh"
export MTOOLS_SKIP_CHECK=1

/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc -s -I"$WATT/inc" -o "$WORK/dj_err.exe" "$WATT/util/errnos.c"
"$NASM" -f bin -o "$WORK/poweroff.com" "$ROOT/tests/mdvmm/poweroff.asm"

cd "$WORK"
printf 'SHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
printf '@ECHO OFF\r\nA:\\DJ_ERR.EXE -s > A:\\SYSERR.C\r\nA:\\DJ_ERR.EXE -e > A:\\DJGPP.ERR\r\nA:\\POWEROFF.COM\r\n' > autoexec.bat
"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=1 -D_SPI=2880 \
  -D_SPF=9 -D_NUMROOT=224 -D_UNIT=00h -o disk.img
mcopy -i disk.img /opt/fdos/bin/command.com ::COMMAND.COM
mcopy -i disk.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i disk.img dj_err.exe ::DJ_ERR.EXE
mcopy -i disk.img /opt/cwsdpmi/bin/CWSDPMI.EXE ::CWSDPMI.EXE
mcopy -i disk.img poweroff.com ::POWEROFF.COM

timeout --foreground 120 "$QEMU" -m 32 -drive file=disk.img,format=raw,if=floppy \
  -display none -net none -no-reboot </dev/null || true

mkdir -p "$OUT"
mtype -i disk.img ::SYSERR.C | sed 's/\r$//' > "$OUT/syserr.c"
mtype -i disk.img ::DJGPP.ERR | sed 's/\r$//' > "$OUT/djgpp.err"
wc -l "$OUT/syserr.c" "$OUT/djgpp.err"
grep -q "SYS_ERRLIST" "$OUT/syserr.c" && grep -q "define" "$OUT/djgpp.err"
echo "watt32 errno tables: done"
