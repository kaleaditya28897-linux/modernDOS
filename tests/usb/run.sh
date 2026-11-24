#!/bin/bash
# USB keyboard test on UHCI controllers (runs inside the build container).
#
# Boots modernDOS from UEFI (OVMF) three times and types at the DOS prompt
# through a USB keyboard (QEMU sendkey); the screen comes back over the
# serial mirror of the console:
#   - keyboard and mouse directly on a UHCI controller
#   - keyboard and mouse behind a hub
#   - like an older Mac: an EHCI controller with UHCI companions, the
#     keyboard behind a hub on an EHCI port (the ports must fall back to
#     the companions), booted from a stick on that controller, no PS/2
#     controller
#
# Usage: tests/usb/run.sh [kernel.sys]

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
"$NASM" -f bin -o "$WORK/poweroff.com" "$ROOT/tests/mdvmm/poweroff.asm"
cd "$WORK"
printf 'SHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
printf '@ECHO OFF\r\nECHO USB TEST READY\r\n' > autoexec.bat
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
mcopy -i boot.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i boot.img poweroff.com ::POWEROFF.COM
dd if=/dev/zero of=esp.img bs=1M count=16 status=none
mformat -i esp.img -h 64 -s 32 -t 16 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MODERNDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img boot.img ::/MODERNDOS/BOOT.IMG

fail=0
run() {   # run name "log markers|..." -- qemu args
  local name=$1 markers=$2; shift 2
  rm -f debug.log serial.log mon.sock
  python3 - <<'PY' &
import socket, time
def text(path):
    try:
        return open(path, "rb").read()
    except OSError:
        return b""
names = {" ": "spc", ":": "shift-semicolon", "\\": "backslash", "\n": "ret", "-": "minus"}
def key(c):
    if c in names:
        return names[c]
    return "shift-" + c.lower() if c.isupper() else c
end = time.time() + 100
while time.time() < end and b"USB TEST READY" not in text("serial.log"):
    time.sleep(0.5)
time.sleep(2)
try:
    s = socket.socket(socket.AF_UNIX); s.connect("mon.sock")
    for c in "ECHO Typed: aB3-usb\nA:\\POWEROFF\n":
        s.sendall(("sendkey %s\n" % key(c)).encode())
        time.sleep(0.15)
    time.sleep(1); s.close()
except OSError:
    pass
PY
  local typer=$!
  timeout --foreground 120 qemu-system-x86_64 -m 256 -bios "$OVMF" -display none -net none \
    "$@" \
    -debugcon file:debug.log -serial file:serial.log -no-reboot \
    -monitor unix:mon.sock,server,nowait </dev/null || true
  kill $typer 2>/dev/null || true
  echo "-- $name"
  [ -n "$KEEP" ] && cp debug.log "$KEEP/usb-${name%% *}.log" && cp serial.log "$KEEP/usb-${name%% *}.serial"
  grep -a "uhci:\|ehci:\|PANIC" debug.log | sed 's/\r$//; s/^/   /'
  local IFS='|' m
  for m in $markers; do
    if grep -aq "$m" debug.log; then echo "  ok   $name: $m"; else
      echo "  FAIL $name: log lacks '$m'"; fail=1; fi
  done
  if grep -aq "MDVMM core:" debug.log && grep -aq "USB TEST READY" serial.log &&
     sed 's/\r//g' serial.log | grep -aq "^Typed: aB3-usb$"; then
    echo "  ok   $name: typed text arrived at the DOS prompt"; else
    echo "  FAIL $name: typed text did not arrive"; fail=1; fi
}

run "UHCI" "uhci: controller 0 port [12]: keyboard|uhci: controller 0 port [12]: mouse" \
  -drive file=esp.img,format=raw,if=ide \
  -device piix3-usb-uhci,id=u -device usb-kbd,bus=u.0,port=1 -device usb-mouse,bus=u.0,port=2
run "UHCI, hub" "uhci: hub at address 1, 8 ports|uhci: hub 01 port 02: keyboard|uhci: hub 01 port 03: mouse" \
  -drive file=esp.img,format=raw,if=ide \
  -device piix3-usb-uhci,id=u -device usb-hub,bus=u.0,port=1 \
  -device usb-kbd,bus=u.0,port=1.2 -device usb-mouse,bus=u.0,port=1.3
run "EHCI with companions (Mac-like)" "ehci: controller at .* left to its UHCI companions|uhci: hub at address 1|: keyboard" \
  -machine q35,i8042=off \
  -device ich9-usb-ehci1,id=e -device ich9-usb-uhci1,masterbus=e.0,firstport=0 \
  -device ich9-usb-uhci2,masterbus=e.0,firstport=2 -device ich9-usb-uhci3,masterbus=e.0,firstport=4 \
  -drive if=none,id=stick,format=raw,file=esp.img -device usb-storage,bus=e.0,port=1,drive=stick \
  -device usb-hub,bus=e.0,port=3 -device usb-kbd,bus=e.0,port=3.1 -device usb-mouse,bus=e.0,port=3.4

if [ $fail = 0 ]; then echo "USB TEST: success"; exit 0; else echo "USB TEST: failure"; exit 1; fi
