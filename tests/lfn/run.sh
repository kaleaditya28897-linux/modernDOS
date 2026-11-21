#!/bin/bash
# modernDOS LFN integration test (runs inside the build container).
#
#  1. builds tests/lfn/lfntest.exe (ia16-gcc + libi86)
#  2. builds a bootable FAT12 floppy with the kernel (same boot sector
#     and image tools as test/test.sh), SHELL=LFNTEST.EXE and a long
#     named file written by mtools
#  3. boots it in QEMU; the test logs to the debug console (port E9h)
#  4. checks RESULT.TXT, verifies the long names from the host side with
#     mtools and runs fsck.fat on the image
#
# Usage: tests/lfn/run.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TEST="$ROOT/test"
. "$TEST/cfg.sh"

echo "== building lfntest.exe"
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/lfntest.exe" "$HERE/lfntest.c" -li86

DJGPP=/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc
if [ -x "$DJGPP" ]; then
  echo "== building djtest.exe (DJGPP)"
  "$DJGPP" -O2 -o "$WORK/djtest.exe" "$HERE/djtest.c"
fi

echo "== building boot floppy"
cd "$WORK"
printf 'failure\r\n' > result.txt
printf 'shell=a:\\lfntest.exe\r\n' > fdconfig.sys
cp "$WORK/lfntest.exe" test.com          # placeholder payload name
"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys,result.txt" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=1 -D_SPI=2880 \
  -D_SPF=9 -D_NUMROOT=224 -D_UNIT=00h -o disk.img

export MTOOLS_SKIP_CHECK=1
mcopy -i disk.img lfntest.exe ::LFNTEST.EXE
if [ -f djtest.exe ]; then
  mcopy -i disk.img djtest.exe ::DJTEST.EXE
  mcopy -i disk.img "$(find /opt/cwsdpmi -iname cwsdpmi.exe | head -1)" ::CWSDPMI.EXE
fi
printf 'hello from linux\n' > linux.txt
mcopy -i disk.img linux.txt "::Created On Linux.txt"

echo "== booting in QEMU"
timeout --foreground 120 "$QEMU" -drive file=disk.img,format=raw,if=floppy -display none \
  -debugcon stdio -no-reboot </dev/null || true

echo "== host side checks"
fail=0
result=$(mtype -i disk.img ::RESULT.TXT 2>/dev/null || true)
[ "$result" = success ] || { echo "RESULT.TXT says: '$result'"; fail=1; }

expect() {  # expect <path> <content>
  got=$(mtype -i disk.img "$1" 2>/dev/null || echo "<missing>")
  if [ "$got" = "$2" ]; then echo "  ok   $1"; else
    echo "  FAIL $1: got '$got'"; fail=1; fi
}
absent() {
  if mtype -i disk.img "$1" >/dev/null 2>&1; then
    echo "  FAIL $1 should not exist"; fail=1; else echo "  ok   $1 absent"; fi
}
expect "::/Long Directory Name/Keep This File.txt" "written by modernDOS"
expect "::/Long Directory Name/My Document With A Long Name.txt" "first file"
expect "::/Long Directory Name/Nested Folder/Moved File.txt" "case"
absent "::/Long Directory Name/My Document With A Long Name 2.txt"
absent "::/Long Directory Name/Many"
echo "  directory listing as seen by Linux:"
mdir -i disk.img "::/Long Directory Name" | sed 's/^/    /'

if command -v fsck.fat >/dev/null; then
  echo "== fsck.fat"
  if fsck.fat -n -V disk.img; then echo "  ok   file system clean"; else
    echo "  FAIL fsck.fat reported problems"; fail=1; fi
fi

if [ $fail = 0 ]; then echo "LFN TEST: success"; else echo "LFN TEST: failure"; fi
exit $fail
