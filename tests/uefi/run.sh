#!/bin/bash
# UEFI boot test (runs inside the build container).
#
# OVMF boots \EFI\BOOT\BOOTX64.EFI from a FAT ESP. The loader starts the
# MDVMM core, which provides a PC BIOS (text console on the GOP
# framebuffer, keyboard, timer, INT 13h on \MODERNDOS\BOOT.IMG) and boots
# the modernDOS floppy image in V86 mode. CONFIG.SYS loads HIMEMX on the
# extended memory the virtual BIOS reports (DOS=HIGH). AUTOEXEC.BAT runs:
#   MDVMM    - no monitor to load: sets up DOS sessions
#   V86CHK   - V86 mode, MDVMM API
#   XMSTEST  - XMS moves (INT 15h AH=87h through the extended memory window)
#   LFNTEST  - long filename suite (heavy INT 21h, timer, disk)
#   DJTEST   - DJGPP program, MDVMM as DPMI host
#   DPMITEST - DPMI host test
#   MDVMM NEW BG.BAT + SESSTEST /FG - a second session; the harness presses
#              Ctrl+Alt+F2/F1
#   DISKTEST C - the AHCI hard disk (64 MB, MBR, FAT16) as drive C:
# Afterwards the hard disk image is checked from Linux: fsck, the 1 MB
# file DISKTEST wrote, FROMDOS.TXT.
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
for p in v86chk poweroff xmstest; do
  "$NASM" -f bin -o "$WORK/$p.com" "$ROOT/tests/mdvmm/$p.asm"
done
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/lfntest.exe" "$ROOT/tests/lfn/lfntest.c" -li86
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/sesstest.exe" "$ROOT/tests/mdvmm/sesstest.c" -li86
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/disktest.exe" "$HERE/disktest.c" -li86
DJGPP=/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc
"$DJGPP" -O2 -o "$WORK/djtest.exe" "$ROOT/tests/lfn/djtest.c"
"$DJGPP" -O2 -o "$WORK/dpmitest.exe" "$ROOT/tests/mdvmm/dpmitest.c"

echo "== building the DOS boot image"
cd "$WORK"
printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
cat > autoexec.bat <<'EOF'
@ECHO OFF
ECHO Hello from DOS on UEFI
A:\MDVMM.COM
IF ERRORLEVEL 1 GOTO END
A:\V86CHK.COM
IF ERRORLEVEL 1 GOTO END
A:\XMSTEST.COM
IF ERRORLEVEL 1 GOTO END
A:\LFNTEST.EXE /T
IF ERRORLEVEL 1 GOTO END
A:\DJTEST.EXE
IF ERRORLEVEL 1 GOTO END
A:\DPMITEST.EXE
IF ERRORLEVEL 1 GOTO END
A:\MDVMM.COM NEW A:\BG.BAT
IF ERRORLEVEL 1 GOTO END
A:\SESSTEST.EXE /FG
IF ERRORLEVEL 1 GOTO END
DIR C:\
A:\DISKTEST.EXE C
IF ERRORLEVEL 1 GOTO END
DIR A:\ /W
ECHO UEFI BOOT: success
GOTO PROMPT
:END
A:\POWEROFF.COM
:PROMPT
EOF
sed -i 's/$/\r/' autoexec.bat
printf '@ECHO OFF\r\nA:\\SESSTEST.EXE /BG\r\nECHO done>A:\\BGDONE.TXT\r\n' > bg.bat

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
mcopy -i boot.img /opt/fdos/bin/himemx.exe ::HIMEMX.EXE
mcopy -i boot.img "$ROOT/mdvmm/build/MDVMM.COM" ::MDVMM.COM
mcopy -i boot.img xmstest.com ::XMSTEST.COM
mcopy -i boot.img dpmitest.exe ::DPMITEST.EXE
mcopy -i boot.img sesstest.exe ::SESSTEST.EXE
mcopy -i boot.img bg.bat ::BG.BAT
mcopy -i boot.img disktest.exe ::DISKTEST.EXE

echo "== building the AHCI hard disk"
HD_SECTORS=131072
dd if=/dev/zero of=hd.img bs=512 count=$HD_SECTORS status=none
python3 - hd.img $HD_SECTORS <<'PY'
import struct, sys
# MBR: one FAT16 partition (type 06h) from sector 2048 to the end
path, total = sys.argv[1], int(sys.argv[2])
entry = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x06, b"\xfe\xff\xff", 2048, total - 2048)
with open(path, "r+b") as f:
    f.seek(0x1be); f.write(entry)
    f.seek(0x1fe); f.write(b"\x55\xaa")
PY
mformat -i hd.img@@1M -T $((HD_SECTORS - 2048)) -h 255 -s 63 -H 2048 -v HARDDISK ::
printf 'hello from linux\n' > hello.txt
mcopy -i hd.img@@1M hello.txt ::HELLO.TXT
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
# keyboard: Ctrl+Alt+F2/F1 while the sessions test runs, then (once the
# batch is done) type at the prompt
python3 - "$WORK" <<'PY' &
import os, socket, sys, time
work = sys.argv[1]
serial, sock = os.path.join(work, "serial.log"), os.path.join(work, "mon.sock")
debug = os.path.join(work, "debug.log")
def text(path):
    try:
        return open(path, "rb").read()
    except OSError:
        return b""
def send(cmd):
    s = socket.socket(socket.AF_UNIX); s.connect(sock)
    s.sendall((cmd + "\n").encode()); time.sleep(0.15); s.close()
deadline = time.time() + 170
while time.time() < deadline and b"sessions test" not in text(debug):
    time.sleep(0.2)
i = 0
while time.time() < deadline and b"SESSIONS:" not in text(debug):
    try:
        send("sendkey " + ["ctrl-alt-f2", "ctrl-alt-f1"][i % 2])
    except OSError:
        break
    i += 1
    time.sleep(1.5)
names = {" ": "spc", ":": "shift-semicolon", "!": "shift-1", "~": "shift-grave_accent",
         "\\": "backslash", "\n": "ret"}
def key(c):
    if c in names:
        return names[c]
    return "shift-" + c.lower() if c.isupper() else c
while time.time() < deadline and b"UEFI BOOT: success" not in text(serial):
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
timeout --foreground 180 qemu-system-x86_64 -m 256 -bios "$OVMF" \
  -drive file=esp.img,format=raw,if=ide -net none -display none \
  -device ahci,id=ahci -drive id=hd,file=hd.img,format=raw,if=none \
  -device ide-hd,drive=hd,bus=ahci.0 \
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
              "vm: sessions enabled" "v86chk: protected mode bit set" \
              "xmstest: 4K round trip" "RESULT: PASSED" "dpmi: 32 bit client" \
              "DJGPP: 0 failures" "DPMI: 0 failures" "vm: session 1 ended" \
              "SESSIONS: 0 failures" "ahci: port 0: QEMU HARDDISK" \
              "DISKTEST: 0 failures"; do
  if grep -q "$marker" debug.log; then echo "  ok   log: $marker"; else
    echo "  FAIL log missing: $marker"; fail=1; fi
done
for marker in "modernDOS kernel" "Hello from DOS on UEFI" "UEFI BOOT: success" \
              "^Typed: aB3!~"; do
  if sed 's/\r//g' serial.log | grep -aq "$marker"; then echo "  ok   screen: $marker"; else
    echo "  FAIL screen missing: $marker"; fail=1; fi
done
# the hard disk, from Linux
dd if=hd.img of=part.img bs=512 skip=2048 status=none
if fsck.fat -n part.img >/dev/null; then echo "  ok   hard disk file system clean"; else
  echo "  FAIL fsck.fat on the hard disk"; fail=1; fi
if mtype -i part.img ::FROMDOS.TXT 2>/dev/null | grep -q "written by DOS through AHCI"; then
  echo "  ok   hard disk: FROMDOS.TXT"; else echo "  FAIL hard disk: FROMDOS.TXT"; fail=1; fi
mcopy -n -i part.img ::DISKTEST/BIG.BIN big.bin 2>/dev/null || true
if python3 - big.bin <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
want = bytes(((o * 7 + (o >> 9) + (o >> 17)) & 0xff) for o in range(1 << 20))
sys.exit(0 if d == want else 1)
PY
then echo "  ok   hard disk: BIG.BIN pattern (1 MB)"; else
  echo "  FAIL hard disk: BIG.BIN"; fail=1; fi
if [ $fail = 0 ]; then echo "UEFI TEST: success"; else echo "UEFI TEST: failure"; exit 1; fi
