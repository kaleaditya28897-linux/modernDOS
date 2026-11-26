#!/bin/bash
# The compatibility test of run.sh on a UEFI boot (runs inside the build
# container): the same programs - real mode, DOS/4GW, PMODE/W, DOS/32A,
# CauseWay, and the DOS-hosted Open Watcom compiler - under the monitor's
# own BIOS (OVMF, BOOTX64.EFI). The boot disk is the 8 MB RAM disk A:; the
# screen comes back over the serial mirror of the console.
#
# Usage: tests/compat/uefi.sh [kernel.sys]

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
export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "== building"
make -s -C "$ROOT/mdvmm"
"$NASM" -f bin -o "$WORK/poweroff.com" "$ROOT/tests/mdvmm/poweroff.asm"
cd "$WORK"
cp "$HERE/hello.c" hello.c
wcl -q -bt=dos -ml -fe=hello16.exe hello.c > w16.log 2>&1 || { cat w16.log; exit 1; }
for ext in dos4g pmodew dos32a causeway; do
  wcl386 -q -bt=dos -l=$ext -fe=h_$ext.exe hello.c > w_$ext.log 2>&1 || { cat w_$ext.log; exit 1; }
done
mv h_causeway.exe h_causew.exe
wcl -q -bt=dos -ml -fe=vga16.exe "$HERE/vga.c" > v16.log 2>&1 || { cat v16.log; exit 1; }
wcl386 -q -bt=dos -l=dos4g -fe=vga32.exe "$HERE/vga.c" > v32.log 2>&1 || { cat v32.log; exit 1; }

printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\r\n' > fdconfig.sys
printf '%s\r\n' '@ECHO OFF' 'SET WATCOM=A:\W' 'SET INCLUDE=A:\W\H' 'PATH A:\;A:\W\BINW' \
  'ECHO === real mode' 'HELLO16.EXE' \
  'ECHO === DOS/4GW' 'H_DOS4G.EXE' \
  'ECHO === PMODE/W' 'H_PMODEW.EXE' \
  'ECHO === DOS/4GW again' 'H_DOS4G.EXE' \
  'ECHO === compiled here' 'COPY HELLO.C MADE.C >NUL' 'WCL386 -q -bt=dos -l=dos4g MADE.C' 'MADE.EXE' \
  'ECHO === DOS/32A' 'H_DOS32A.EXE' \
  'ECHO === CauseWay' 'H_CAUSEW.EXE' \
  'ECHO === VGA 16' 'VGA16.EXE' \
  'ECHO === VGA 32' 'VGA32.EXE' \
  'ECHO === end' 'A:\POWEROFF.COM' > autoexec.bat
"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -D_CHS_SECTORS=63 -D_CHS_HEADS=2 -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=8 -D_SPI=16380 \
  -D_SPF=6 -D_NUMROOT=224 -D_CHS_SECTORS=63 -D_CHS_HEADS=2 -D_UNIT=00h -o boot.img
for f in /opt/fdos/bin/command.com /opt/fdos/bin/himemx.exe autoexec.bat poweroff.com \
         hello16.exe h_dos4g.exe h_pmodew.exe h_dos32a.exe h_causew.exe hello.c vga16.exe vga32.exe \
         $WATCOM/binw/dos4gw.exe $WATCOM/binw/pmodew.exe $WATCOM/binw/dos32a.exe; do
  mcopy -i boot.img "$f" "::$(basename "$f" | tr a-z A-Z)"
done
mmd -i boot.img ::/W ::/W/BINW ::/W/H ::/W/LIB386 ::/W/LIB386/DOS
for f in wcl386.exe wcc386.exe wlink.exe wlink.lnk dos4gw.exe wstub.exe wcc38601.int; do
  mcopy -i boot.img $WATCOM/binw/$f ::/W/BINW/
done
for f in stdio.h stdlib.h string.h dos.h i86.h _comdef.h malloc.h; do
  [ -f $WATCOM/h/$f ] && mcopy -i boot.img $WATCOM/h/$f ::/W/H/
done
mcopy -i boot.img $WATCOM/lib386/dos/clib3r.lib $WATCOM/lib386/dos/emu387.lib ::/W/LIB386/DOS/
mcopy -i boot.img $WATCOM/lib386/math387r.lib ::/W/LIB386/
dd if=/dev/zero of=esp.img bs=1M count=24 status=none
mformat -i esp.img -h 64 -s 32 -t 24 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img boot.img ::/MDOS/BOOT.IMG

echo "== booting"
python3 "$HERE/vgawatch.py" debug.log mon.sock "$WORK/vga" &
WATCH=$!
timeout --foreground 300 qemu-system-x86_64 -m 256 -bios "$OVMF" -display none \
  -drive file=esp.img,format=raw,if=ide -nic user,model=e1000 \
  -debugcon file:debug.log -serial file:serial.log -monitor unix:mon.sock,server,nowait \
  -no-reboot </dev/null || true
kill $WATCH 2>/dev/null || true
sed 's/\r//g; s/\x1b\[[0-9;]*[A-Za-z]//g' serial.log | tr -d '\000' | grep -av '^$' > screen.txt
sed -n '/^=== real mode/,$p' screen.txt | grep -av "^  ok " | head -60 | sed 's/^/   /'
grep -a "^v86: a program\|^vga: \|PANIC" -A6 debug.log | head -12 | sed "s/^/   monitor: /" || true

fail=0
section() { sed -n "/^=== $1 *\$/,/^=== [^ ]* *[^ ]* *\$/p" screen.txt; }
for kind in "real mode" "DOS\\/4GW" "PMODE\\/W" "DOS\\/4GW again" "compiled here" "DOS\\/32A" "CauseWay"; do
  name=$(echo "$kind" | tr -d '\\')
  if section "$kind" | grep -aq "hello: all 5 ok"; then echo "  ok   UEFI: $name"; else echo "  FAIL UEFI: $name"; fail=1; fi
done
for bits in 16 32; do
  if section "VGA $bits" | grep -aq "vga: $bits bit, mode 13h, palette 63/0/0: all ok" &&
     python3 "$HERE/vgacheck.py" vga$bits.ppm; then
    echo "  ok   UEFI: VGA mode 13h, $bits bit: palette, pixels, the picture on the framebuffer"
  else echo "  FAIL UEFI: VGA mode 13h, $bits bit"; fail=1; fi
done
grep -aq "^=== end" screen.txt && echo "  ok   UEFI: ran to the end" || { echo "  FAIL UEFI: did not run to the end"; fail=1; }
if [ $fail = 0 ]; then echo "COMPAT UEFI TEST: success"; exit 0; else echo "COMPAT UEFI TEST: failure"; exit 1; fi
