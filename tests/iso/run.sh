#!/bin/bash
# Release ISO boot test (runs inside the build container).
# Boots the ISO three ways and waits for the monitor to be up:
#   UEFI from CD, UEFI from a USB stick (the ISO written with dd),
#   BIOS from CD (El Torito hard disk emulation of the BIOS disk image)
# UEFI boots also get a SATA disk, which must come up read-only (the ISO
# has no \MDOS\DISKRW).
# Then the stick as on a real machine: the ISO written to a larger,
# writable stick; after the boot \MDOS\HWINFO.TXT and BOOTLOG.TXT
# must be on its EFI partition (found through the GPT, as an OS would),
# also after a second boot (the files are replaced). And a boot under
# conditions like an older Mac: stick on EHCI, no GOP graphics, no PS/2
# keyboard controller.
#
# Usage: tests/iso/run.sh [modernDOS.iso]

set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ISO=$(realpath "${1:-$ROOT/_output/release/modernDOS-0.0.1-alpha.iso}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
OVMF=/usr/share/ovmf/OVMF.fd
cd "$WORK"
dd if=/dev/zero of=hd.img bs=1M count=16 status=none
fail=0

# boot name markers... -- qemu options
boot() {
  local name=$1 markers=$2; shift 2
  rm -f debug.log
  timeout --foreground 120 qemu-system-x86_64 -m 256 -display none -net none \
    -debugcon file:debug.log -no-reboot "$@" </dev/null &
  local qemu=$!
  python3 - debug.log "$markers" <<'PY'
import re, sys, time
log, markers = sys.argv[1], sys.argv[2].split("|")
end = time.time() + 110
while time.time() < end:
    try:
        text = open(log, "rb").read().decode("latin-1")
    except OSError:
        text = ""
    if all(re.search(m, text) for m in markers):
        break
    time.sleep(0.5)
PY
  kill $qemu 2>/dev/null || true
  wait $qemu 2>/dev/null || true
  local IFS='|' m
  for m in $markers; do
    if grep -aq "$m" debug.log; then echo "  ok   $name: $m"; else
      echo "  FAIL $name: missing '$m'"; fail=1; fi
  done
}

UEFI_MARKERS="MDVMM core:|bios: disk image|v86: entering VM1|vm: sessions enabled|bios: hard disks are read-only|bios: mouse driver installed"
SATA="-device ahci,id=ahci -drive id=hd,file=hd.img,format=raw,if=none -device ide-hd,drive=hd,bus=ahci.0"

echo "== UEFI, CD"
boot "UEFI CD" "$UEFI_MARKERS" -bios "$OVMF" -cdrom "$ISO" $SATA
echo "== UEFI, USB stick"
boot "UEFI USB" "$UEFI_MARKERS" -bios "$OVMF" \
  -drive if=none,id=stick,format=raw,readonly=on,file="$ISO" \
  -device qemu-xhci -device usb-storage,drive=stick $SATA
echo "== BIOS, CD"
boot "BIOS CD" "MDVMM core:|v86: entering VM1|vm: sessions enabled" -cdrom "$ISO" -boot d

# ------------------------------------------------------------------
# the EFI system partition of a stick image: offset in bytes, via the GPT
esp_offset() {
  python3 - "$1" <<'PY'
import struct, sys
f = open(sys.argv[1], "rb")
f.seek(512)
h = f.read(92)
assert h[:8] == b"EFI PART", "no GPT"
lba, n, size = struct.unpack_from("<QII", h, 72)
esp = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
for i in range(n):
    f.seek(lba * 512 + i * size)
    e = f.read(size)
    if e[:16] == esp:
        print(struct.unpack_from("<Q", e, 32)[0] * 512)
        break
PY
}

# files on the stick's EFI partition after a boot
stick_files() {   # stick_files name image expected-in-hwinfo...
  local name=$1 img=$2 off m; shift 2
  off=$(esp_offset "$img")
  if [ -z "$off" ]; then echo "  FAIL $name: no EFI system partition in the GPT"; fail=1; return; fi
  MTOOLS_SKIP_CHECK=1 mtype -i "$img@@$off" ::/MDOS/HWINFO.TXT 2>/dev/null | tr -d '\r' > hwinfo.txt || true
  MTOOLS_SKIP_CHECK=1 mtype -i "$img@@$off" ::/MDOS/BOOTLOG.TXT 2>/dev/null | tr -d '\r' > bootlog.txt || true
  for m in "$@" "PCI devices" "End of report"; do
    if grep -q "$m" hwinfo.txt; then echo "  ok   $name: HWINFO.TXT: $m"; else
      echo "  FAIL $name: HWINFO.TXT lacks '$m'"; fail=1; fi
  done
  for m in "Hardware report written" "Starting the monitor"; do
    if grep -q "$m" bootlog.txt; then echo "  ok   $name: BOOTLOG.TXT: $m"; else
      echo "  FAIL $name: BOOTLOG.TXT lacks '$m'"; fail=1; fi
  done
  if [ "$(MTOOLS_SKIP_CHECK=1 mdir -i "$img@@$off" ::/MDOS | grep -c 'HWINFO\|BOOTLOG')" = 2 ]; then
    echo "  ok   $name: one HWINFO.TXT and one BOOTLOG.TXT"; else
    echo "  FAIL $name: directory:"; MTOOLS_SKIP_CHECK=1 mdir -i "$img@@$off" ::/MDOS; fail=1; fi
  if command -v fsck.vfat > /dev/null; then
    dd if="$img" of=esp.part bs=512 skip=$((off / 512)) count=61440 status=none
    if fsck.vfat -n esp.part > fsck.log 2>&1; then echo "  ok   $name: the FAT file system is clean"; else
      echo "  FAIL $name: fsck.vfat:"; sed 's/^/       /' fsck.log | head; fail=1; fi
  fi
}

MON_MARKERS="MDVMM core:|timer: ok - the 8254 ticks and its interrupt reaches the CPU|v86: entering VM1|vm: sessions enabled"

echo "== UEFI, USB stick (xHCI), larger and writable"
cp "$ISO" stick.img
truncate -s 256M stick.img
boot "USB stick" "fbcon: [0-9]*x[0-9]* framebuffer|$MON_MARKERS" -bios "$OVMF" \
  -drive if=none,id=stick,format=raw,file=stick.img \
  -device qemu-xhci -device usb-storage,drive=stick $SATA
stick_files "USB stick" stick.img "GOP: mode" "Firmware: "
echo "== the same stick, booted again"
boot "USB stick, 2nd boot" "$MON_MARKERS" -bios "$OVMF" \
  -drive if=none,id=stick,format=raw,file=stick.img \
  -device qemu-xhci -device usb-storage,drive=stick $SATA
stick_files "USB stick, 2nd boot" stick.img "GOP: mode"

echo "== like an older Mac: stick on EHCI, no GOP, no PS/2 controller"
cp "$ISO" stick2.img
truncate -s 256M stick2.img
boot "Mac-like" "fbcon: no usable framebuffer|$MON_MARKERS" -machine q35,i8042=off -bios "$OVMF" -vga none \
  -drive if=none,id=stick,format=raw,file=stick2.img \
  -device usb-ehci -device usb-storage,drive=stick
stick_files "Mac-like" stick2.img "GOP: none"
grep -a "apic:\|hpet:\|timer:" debug.log | sed 's/\r$//; s/^/   /'

if [ $fail = 0 ]; then echo "ISO TEST: success"; else echo "ISO TEST: failure"; exit 1; fi
