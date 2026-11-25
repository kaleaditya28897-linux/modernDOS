#!/bin/bash
# HTTPS from modernDOS to real sites on the internet (runs inside the build
# container; needs internet access, so it is not part of mdtest.sh).
#
# A UEFI boot (OVMF, iPXE's e1000 driver, PKTDRV, Watt-32) runs HTTPGET
# (HTTP client + BearSSL) against a few sites: pages must arrive, and
# servers with a wrong certificate must be refused.
#
# Usage: tests/pkg/internet.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TEST="$ROOT/test"
. "$TEST/cfg.sh"
OVMF=/usr/share/ovmf/OVMF.fd
export MTOOLS_SKIP_CHECK=1

echo "== building"
make -s -C "$ROOT/mdvmm"
make -s -C "$ROOT/net/pkg" all
"$NASM" -f bin -o "$WORK/poweroff.com" "$ROOT/tests/mdvmm/poweroff.asm"
cd "$WORK"
printf 'SHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
printf '%s\r\n' '@ECHO OFF' 'SET WATTCP.CFG=A:\' 'A:\PKTDRV.COM' \
  'A:\HTTPGET.EXE https://example.com/ A:\EX.HTM' \
  'A:\HTTPGET.EXE https://en.wikipedia.org/wiki/DOS' 'A:\HTTPGET.EXE https://lite.duckduckgo.com/lite/' \
  'A:\HTTPGET.EXE https://github.com/' 'A:\HTTPGET.EXE http://neverssl.com/' \
  'A:\HTTPGET.EXE https://expired.badssl.com/' 'A:\HTTPGET.EXE https://wrong.host.badssl.com/' \
  'A:\HTTPGET.EXE https://self-signed.badssl.com/' \
  'ECHO NET TEST DONE' 'A:\POWEROFF.COM' > autoexec.bat
printf 'my_ip = dhcp\r\n' > wattcp.cfg
"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=2 -D_SPI=5760 \
  -D_SPF=9 -D_NUMROOT=224 -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -D_UNIT=00h -o boot.img
for f in /opt/fdos/bin/command.com autoexec.bat wattcp.cfg poweroff.com \
         "$ROOT/mdvmm/build/PKTDRV.COM"; do
  mcopy -i boot.img "$f" "::$(basename "$f" | tr a-z A-Z)"
done
mcopy -i boot.img "$ROOT/net/pkg/build/httpget.exe" ::HTTPGET.EXE
dd if=/dev/zero of=esp.img bs=1M count=16 status=none
mformat -i esp.img -h 64 -s 32 -t 16 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img boot.img ::/MDOS/BOOT.IMG

echo "== booting"
timeout --foreground 400 qemu-system-x86_64 -m 256 -bios "$OVMF" -display none \
  -drive file=esp.img,format=raw,if=ide -nic user,model=e1000 \
  -debugcon file:debug.log -serial file:serial.log -no-reboot </dev/null || true
sed 's/\r//g; s/\x1b\[[0-9;]*[A-Za-z]//g' serial.log | tr -d '\000' | grep -av '^$' > screen.txt
grep -a "HTTPGET\|Example\|DONE\|EX.HTM" screen.txt | sed 's/^/   /'
fail=0
[ "$(grep -ac "HTTPGET: ok" screen.txt)" = 5 ] || { echo "  FAIL not all 5 pages arrived"; fail=1; }
for marker in "certificate is expired" "certificate is for another name" \
              "not from a known authority" "NET TEST DONE"; do
  if grep -aq "$marker" screen.txt; then echo "  ok   $marker"; else echo "  FAIL missing: $marker"; fail=1; fi
done
if [ $fail = 0 ]; then echo "INTERNET TEST: success"; exit 0; else echo "INTERNET TEST: failure"; exit 1; fi
