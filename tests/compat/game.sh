#!/bin/bash
# A game off the shelf on both editions (runs inside the build container):
# Kiloblaster (Epic MegaGames 1992, freeware since 2008; the copy of the
# FreeDOS games repository, in the build image under /opt/games). A real
# mode program with its own keyboard and timer handlers, on the VGA in its
# unchained 256 colour mode (four planes, "mode X").
#
# It is started on the BIOS edition (SeaBIOS, the emulated VGA card: the
# reference) and on a UEFI boot (OVMF, the monitor's own VGA on the
# framebuffer). A key takes it past its first screen; then pictures are
# taken. Screens that stand still (loading, title, main menu) must look the
# same on both.
#
# Usage: tests/compat/game.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
GAME=${GAMEDIR:-/opt/games/kiloblas}
WORK=$(mktemp -d)
VER=0.0.0-gametest
OUT="$ROOT/_output/release"
trap 'kill $QPID 2>/dev/null || true; rm -rf "$WORK" "$OUT/modernDOS-$VER"*' EXIT
TEST="$ROOT/test"
. "$TEST/cfg.sh"
OVMF=/usr/share/ovmf/OVMF.fd
export MTOOLS_SKIP_CHECK=1
[ -f "$GAME/KFILE1.EXE" ] || { echo "no game in $GAME (build/games.sh)"; exit 1; }

echo "== building"
make -s -C "$ROOT/mdvmm"
cd "$WORK"
"$ROOT/release/build.sh" $VER > build.log 2>&1 || { tail -20 build.log; exit 1; }
"$NASM" -f bin -o poweroff.com "$ROOT/tests/mdvmm/poweroff.asm"

# the game's disk: one FAT16 partition at 1 MB
make_disk() {
  dd if=/dev/zero of="$1" bs=512 count=131072 status=none
  python3 - "$1" 131072 <<'PY'
import struct, sys
path, total = sys.argv[1], int(sys.argv[2])
entry = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x06, b"\xfe\xff\xff", 2048, total - 2048)
with open(path, "r+b") as f:
    f.seek(0x1be); f.write(entry)
    f.seek(0x1fe); f.write(b"\x55\xaa")
PY
  mformat -i "$1@@1M" -T $((131072 - 2048)) -h 255 -s 63 -H 2048 -v GAMES ::
  mmd -i "$1@@1M" ::/KB
  mcopy -i "$1@@1M" "$GAME"/* ::/KB/
}

# BIOS edition: its disk image, the game on a second disk (D:)
cp "$OUT/modernDOS-$VER-bios.img" bios.img
PART=$((1008 * 512))
make_disk bios-hd.img
mtype -i bios.img@@$PART ::AUTOEXEC.BAT > autoexec.bat
printf '%s\r\n' 'D:' 'CD \KB' 'KFILE1.EXE' 'C:\POWEROFF.COM' >> autoexec.bat
mdel -i bios.img@@$PART ::AUTOEXEC.BAT
mcopy -i bios.img@@$PART autoexec.bat ::AUTOEXEC.BAT
mcopy -i bios.img@@$PART poweroff.com ::POWEROFF.COM

# UEFI: a boot disk of its own, the game on an AHCI disk (C:)
make_disk uefi-hd.img
printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nFILES=40\r\nSHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\r\n' > fdconfig.sys
printf '%s\r\n' '@ECHO OFF' 'PATH A:\' 'C:' 'CD \KB' 'KFILE1.EXE' 'A:\POWEROFF.COM' > autoexec.bat
"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=2 -D_SPI=5760 \
  -D_SPF=9 -D_NUMROOT=224 -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -D_UNIT=00h -o boot.img
for f in /opt/fdos/bin/command.com /opt/fdos/bin/himemx.exe autoexec.bat poweroff.com; do
  mcopy -i boot.img "$f" "::$(basename "$f" | tr a-z A-Z)"
done
dd if=/dev/zero of=esp.img bs=1M count=16 status=none
mformat -i esp.img -h 64 -s 32 -t 16 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img boot.img ::/MDOS/BOOT.IMG
printf 'the disks may be written\r\n' > diskrw
mcopy -i esp.img diskrw ::/MDOS/DISKRW

fail=0
play() {   # play name qemu...: pictures into $WORK/name-NN.ppm
  local name=$1; shift
  echo "== $name"
  rm -f debug.log mon.sock
  "$@" -display none -debugcon file:debug.log -monitor unix:mon.sock,server,nowait -no-reboot </dev/null &
  QPID=$!
  python3 "$HERE/gameplay.py" debug.log mon.sock "$WORK/$name" || true
  kill $QPID 2>/dev/null || true
  wait $QPID 2>/dev/null || true
  grep -a "^vga: \|^v86: a program\|^kbd: \|PANIC" -A5 debug.log | head -12 | sed 's/^/   monitor: /' || true
  if grep -aq "PANIC" debug.log; then echo "  FAIL $name: monitor panic"; fail=1; fi
}
play bios qemu-system-i386 -m 128 -drive file=bios.img,format=raw,if=ide -drive file=bios-hd.img,format=raw,if=ide
play uefi qemu-system-x86_64 -m 256 -bios "$OVMF" -drive file=esp.img,format=raw,if=ide \
  -device ahci,id=ahci -drive id=hd,file=uefi-hd.img,format=raw,if=none -device ide-hd,drive=hd,bus=ahci.0

[ -n "$SHOTS" ] && mkdir -p "$SHOTS" && cp "$WORK"/*.ppm "$SHOTS"/ 2>/dev/null || true
if python3 "$HERE/gamecheck.py" "$WORK/bios" "$WORK/uefi"; then
  echo "  ok   the game's still screens look the same on the BIOS edition and on UEFI"
else
  echo "  FAIL the game's screens on UEFI are not those of the BIOS edition"; fail=1
fi
if [ $fail = 0 ]; then echo "GAME TEST: success"; exit 0; else echo "GAME TEST: failure"; exit 1; fi
