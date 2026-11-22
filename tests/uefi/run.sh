#!/bin/bash
# UEFI boot test (runs inside the build container).
#
# OVMF boots \EFI\BOOT\BOOTX64.EFI from a FAT ESP. The loader starts the
# MDVMM core, which provides a PC BIOS (text console on the GOP
# framebuffer, keyboard, timer, INT 13h on \MODERNDOS\BOOT.IMG) and boots
# the modernDOS floppy image in V86 mode. AUTOEXEC.BAT then runs:
#   V86CHK   - V86 mode, MDVMM API
#   LFNTEST  - long filename suite (heavy INT 21h, timer, disk)
#   DJTEST   - DJGPP program, MDVMM as DPMI host
# and leaves FreeCOM at its prompt, where the harness types (QEMU sendkey)
#   ECHO Typed: aB3!~   and then   A:\POWEROFF
# The console is mirrored to COM1 (serial.log); MDVMM and the tests log
# to the QEMU debug console. With SCREEN=file.ppm the GOP screen is saved
# at the end.
#
# Usage: tests/uefi/run.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TEST="$ROOT/test"
. "$TEST/cfg.sh"
OVMF=/usr/share/ovmf/OVMF.fd

echo "== building MDVMM and test programs"
make -s -C "$ROOT/mdvmm"
for p in v86chk poweroff; do
  "$NASM" -f bin -o "$WORK/$p.com" "$ROOT/tests/mdvmm/$p.asm"
done
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/lfntest.exe" "$ROOT/tests/lfn/lfntest.c" -li86
/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc -O2 -o "$WORK/djtest.exe" "$ROOT/tests/lfn/djtest.c"

echo "== building the DOS boot image"
cd "$WORK"
printf 'SHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
cat > autoexec.bat <<'EOF'
@ECHO OFF
ECHO Hello from DOS on UEFI
A:\V86CHK.COM
IF ERRORLEVEL 1 GOTO END
A:\LFNTEST.EXE /T
IF ERRORLEVEL 1 GOTO END
A:\DJTEST.EXE
IF ERRORLEVEL 1 GOTO END
DIR A:\ /W
ECHO UEFI BOOT: success
GOTO PROMPT
:END
A:\POWEROFF.COM
:PROMPT
EOF
sed -i 's/$/\r/' autoexec.bat

"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=1 -D_SPI=2880 \
  -D_SPF=9 -D_NUMROOT=224 -D_UNIT=00h -o boot.img

export MTOOLS_SKIP_CHECK=1
mcopy -i boot.img /opt/fdos/bin/command.com ::COMMAND.COM
mcopy -i boot.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i boot.img v86chk.com ::V86CHK.COM
mcopy -i boot.img poweroff.com ::POWEROFF.COM
mcopy -i boot.img lfntest.exe ::LFNTEST.EXE
mcopy -i boot.img djtest.exe ::DJTEST.EXE
printf 'hello from linux\n' > linux.txt
mcopy -i boot.img linux.txt "::Created On Linux.txt"     # LFNTEST fixture

echo "== building the EFI system partition"
dd if=/dev/zero of=esp.img bs=1M count=32 status=none
mformat -i esp.img -h 64 -s 32 -t 32 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MODERNDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img boot.img ::/MODERNDOS/BOOT.IMG

echo "== booting OVMF in QEMU"
if [ -n "$SCREEN" ]; then
  # save the screen every 2 s until QEMU exits: the last one stays
  python3 - "$WORK" "$SCREEN" <<'PY' &
import os, socket, sys, time
work, out = sys.argv[1], sys.argv[2]
sock = os.path.join(work, "mon.sock")
time.sleep(3)
while True:
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sock)
        s.sendall(("screendump %s.tmp\n" % out).encode()); time.sleep(0.5); s.close()
        if os.path.exists(out + ".tmp"):
            os.replace(out + ".tmp", out)
    except OSError:
        break
    time.sleep(1.5)
PY
  shot_pid=$!
fi
# keyboard: once the batch is done, type at the prompt
python3 - "$WORK" <<'PY' &
import os, socket, sys, time
work = sys.argv[1]
serial, sock = os.path.join(work, "serial.log"), os.path.join(work, "mon.sock")
names = {" ": "spc", ":": "shift-semicolon", "!": "shift-1", "~": "shift-grave_accent",
         "\\": "backslash", "\n": "ret"}
def key(c):
    if c in names:
        return names[c]
    return "shift-" + c.lower() if c.isupper() else c
deadline = time.time() + 110
while time.time() < deadline:
    try:
        if b"UEFI BOOT: success" in open(serial, "rb").read():
            break
    except OSError:
        pass
    time.sleep(0.5)
time.sleep(2)
try:
    s = socket.socket(socket.AF_UNIX); s.connect(sock)
    for c in "ECHO Typed: aB3!~\nA:\\POWEROFF\n":
        s.sendall(("sendkey %s\n" % key(c)).encode())
        time.sleep(0.15)
    time.sleep(1); s.close()
except OSError:
    pass
PY
keys_pid=$!
timeout --foreground 120 qemu-system-x86_64 -m 128 -bios "$OVMF" \
  -drive file=esp.img,format=raw,if=ide -net none -display none \
  -debugcon file:debug.log -serial file:serial.log -no-reboot \
  -monitor unix:mon.sock,server,nowait $QEMU_EXTRA </dev/null || true
kill $keys_pid 2>/dev/null || true
[ -n "$SCREEN" ] && wait $shot_pid 2>/dev/null || true
echo "-- debug console"
sed 's/\r$//' debug.log
echo "-- screen (serial mirror)"
sed 's/\r$//; s/\x1b\[[0-9;]*[A-Za-z]//g' serial.log | tr -d '\000' | grep -a -v '^$' | tail -40

echo "== checks"
fail=0
for marker in "MDVMM core:" "bios: disk image" "fbcon:" "v86: entering VM1" \
              "v86chk: protected mode bit set" "RESULT: PASSED" "dpmi: 32 bit client" \
              "DJGPP: 0 failures"; do
  if grep -q "$marker" debug.log; then echo "  ok   log: $marker"; else
    echo "  FAIL log missing: $marker"; fail=1; fi
done
for marker in "modernDOS kernel" "Hello from DOS on UEFI" "UEFI BOOT: success" \
              "^Typed: aB3!~"; do
  if sed 's/\r//g' serial.log | grep -aq "$marker"; then echo "  ok   screen: $marker"; else
    echo "  FAIL screen missing: $marker"; fail=1; fi
done
if [ $fail = 0 ]; then echo "UEFI TEST: success"; else echo "UEFI TEST: failure"; exit 1; fi
