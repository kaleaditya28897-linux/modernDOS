#!/bin/bash
# Release ISO boot test (runs inside the build container).
# Boots the ISO three ways and waits for the monitor to be up:
#   UEFI from CD, UEFI from a USB stick (the ISO written with dd),
#   BIOS from CD (El Torito floppy emulation)
# UEFI boots also get a SATA disk, which must come up read-only (the ISO
# has no \MODERNDOS\DISKRW).
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
import sys, time
log, markers = sys.argv[1], sys.argv[2].split("|")
end = time.time() + 110
while time.time() < end:
    try:
        text = open(log, "rb").read().decode("latin-1")
    except OSError:
        text = ""
    if all(m in text for m in markers):
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

UEFI_MARKERS="MDVMM core:|bios: disk image|v86: entering VM1|vm: sessions enabled|bios: hard disks are read-only"
SATA="-device ahci,id=ahci -drive id=hd,file=hd.img,format=raw,if=none -device ide-hd,drive=hd,bus=ahci.0"

echo "== UEFI, CD"
boot "UEFI CD" "$UEFI_MARKERS" -bios "$OVMF" -cdrom "$ISO" $SATA
echo "== UEFI, USB stick"
boot "UEFI USB" "$UEFI_MARKERS" -bios "$OVMF" \
  -drive if=none,id=stick,format=raw,readonly=on,file="$ISO" \
  -device qemu-xhci -device usb-storage,drive=stick $SATA
echo "== BIOS, CD"
boot "BIOS CD" "MDVMM core:|v86: entering VM1|vm: sessions enabled" -cdrom "$ISO" -boot d

if [ $fail = 0 ]; then echo "ISO TEST: success"; else echo "ISO TEST: failure"; exit 1; fi
