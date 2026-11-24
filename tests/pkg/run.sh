#!/bin/bash
# Package manager test on modernDOS (runs inside the build container).
#
# A UEFI boot (OVMF, iPXE's e1000 driver, PKTDRV, Watt-32) with PKG.EXE and
# a repository made here and served on 10.0.2.2:8377: PKG UPDATE (signed
# list), PKG INSTALL (SHA-256 checked, unpacked on A:), the installed batch
# file runs, PKG LIST, PKG REMOVE. The screen comes back over the serial
# mirror of the console.
#
# Usage: tests/pkg/run.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
WORK=$(mktemp -d)
trap 'kill $WEB 2>/dev/null || true; rm -rf "$WORK"' EXIT
TEST="$ROOT/test"
. "$TEST/cfg.sh"
OVMF=/usr/share/ovmf/OVMF.fd
export MTOOLS_SKIP_CHECK=1

echo "== building"
make -s -C "$ROOT/mdvmm"
make -s -C "$ROOT/net/pkg" all host
"$NASM" -f bin -o "$WORK/poweroff.com" "$ROOT/tests/mdvmm/poweroff.asm"
cd "$WORK"
mkdir -p src/hello/root/APPS/HELLO
printf 'version=1.2\ndescription=Says hello\n' > src/hello/PKGINFO
printf '@ECHO OFF\r\nECHO Hello from a package\r\n' > src/hello/root/APPS/HELLO/HELLO.BAT
head -c 150000 /dev/urandom > src/hello/root/APPS/HELLO/DATA.BIN
"$ROOT/net/pkg/mkrepo.sh" src repo repo.key > mkrepo.log
KEY=$(sed -n 's/^public key.*: //p' mkrepo.log)
python3 -m http.server 8377 --bind 127.0.0.1 -d repo > /dev/null 2>&1 &
WEB=$!

printf 'SHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
printf '%s\r\n' '@ECHO OFF' 'SET WATTCP.CFG=A:\' 'A:\PKTDRV.COM' 'A:\PKG.EXE UPDATE' \
  'A:\PKG.EXE INSTALL hello' 'CALL A:\APPS\HELLO\HELLO.BAT' 'A:\PKG.EXE LIST' \
  'A:\PKG.EXE REMOVE hello' 'IF NOT EXIST A:\APPS\HELLO\DATA.BIN ECHO files are gone' \
  'ECHO PKG TEST DONE' 'A:\POWEROFF.COM' > autoexec.bat
printf 'my_ip = dhcp\r\n' > wattcp.cfg
printf 'repo = http://10.0.2.2:8377\r\nkey = %s\r\nroot = A:\\\r\n' "$KEY" > pkg.cfg
"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=2 -D_SPI=5760 \
  -D_SPF=9 -D_NUMROOT=224 -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -D_UNIT=00h -o boot.img
for f in /opt/fdos/bin/command.com autoexec.bat wattcp.cfg pkg.cfg poweroff.com \
         "$ROOT/mdvmm/build/PKTDRV.COM"; do
  mcopy -i boot.img "$f" "::$(basename "$f" | tr a-z A-Z)"
done
mcopy -i boot.img "$ROOT/net/pkg/build/pkg.exe" ::PKG.EXE
dd if=/dev/zero of=esp.img bs=1M count=16 status=none
mformat -i esp.img -h 64 -s 32 -t 16 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img boot.img ::/MDOS/BOOT.IMG

echo "== booting"
timeout --foreground 240 qemu-system-x86_64 -m 256 -bios "$OVMF" -display none \
  -drive file=esp.img,format=raw,if=ide -nic user,model=e1000 \
  -debugcon file:debug.log -serial file:serial.log -no-reboot </dev/null || true
sed 's/\r//g; s/\x1b\[[0-9;]*[A-Za-z]//g' serial.log | tr -d '\000' | grep -av '^$' > screen.txt
sed -n '/Package list\|PKG:/,$p' screen.txt | head -30 | sed 's/^/   /'
fail=0
for marker in "Package list updated: 1 packages (signature good)" "Installed hello 1.2: 2 files" \
              "^Hello from a package" "^\[\*\] hello" "Removed hello: 2 files" "^files are gone" "PKG TEST DONE"; do
  if grep -aq "$marker" screen.txt; then echo "  ok   $marker"; else echo "  FAIL missing: $marker"; fail=1; fi
done
if [ $fail = 0 ]; then echo "PKG TEST: success"; exit 0; else echo "PKG TEST: failure"; exit 1; fi
