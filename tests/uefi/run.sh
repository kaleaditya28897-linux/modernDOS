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
#   DISKTEST D - the NVMe disk (96 MB, MBR, FAT16) as drive D:
# Afterwards both disk images are checked from Linux: fsck, the 1 MB
# file DISKTEST wrote, FROMDOS.TXT.
# and leaves FreeCOM at its prompt, where the harness types (QEMU sendkey)
#   ECHO Typed: aB3!~   ECHO R + x held for 1.5 s (repeat)   A:\POWEROFF
# All keys (also the session hotkeys) go to a USB keyboard on an xHCI
# controller: QEMU sends input to the last keyboard that was activated.
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
make -s -C "$ROOT/gui"
for p in v86chk poweroff xmstest; do
  "$NASM" -f bin -o "$WORK/$p.com" "$ROOT/tests/mdvmm/$p.asm"
done
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/lfntest.exe" "$ROOT/tests/lfn/lfntest.c" -li86
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/sesstest.exe" "$ROOT/tests/mdvmm/sesstest.c" -li86
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/disktest.exe" "$HERE/disktest.c" -li86
DJGPP=/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc
"$DJGPP" -O2 -s -o "$WORK/djtest.exe" "$ROOT/tests/lfn/djtest.c"
"$DJGPP" -O2 -s -o "$WORK/dpmitest.exe" "$ROOT/tests/mdvmm/dpmitest.c"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/gfxtest.exe" "$ROOT/tests/gui/gfxtest.c" "$ROOT/gui/build/mdgui.o"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/mousetest.exe" "$ROOT/tests/gui/mousetest.c" "$ROOT/gui/build/mdgui.o"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/wmtest.exe" "$ROOT/tests/gui/wmtest.c" "$ROOT/gui/build/mdgui.o"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/uitest.exe" "$ROOT/tests/gui/uitest.c" "$ROOT/gui/build/mdgui.o"

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
A:\GFXTEST.EXE
IF ERRORLEVEL 1 GOTO END
A:\MOUSETEST.EXE
IF ERRORLEVEL 1 GOTO END
A:\WMTEST.EXE
IF ERRORLEVEL 1 GOTO END
A:\UITEST.EXE
IF ERRORLEVEL 1 GOTO END
A:\MDDESK.EXE /TEST
IF ERRORLEVEL 1 GOTO END
A:\MDDESK.EXE /TEST /EXIT
IF ERRORLEVEL 1 GOTO END
DIR C:\
A:\DISKTEST.EXE C
IF ERRORLEVEL 1 GOTO END
DIR D:\
A:\DISKTEST.EXE D
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
mcopy -i boot.img gfxtest.exe ::GFXTEST.EXE
mcopy -i boot.img mousetest.exe ::MOUSETEST.EXE
mcopy -i boot.img wmtest.exe ::WMTEST.EXE
mcopy -i boot.img uitest.exe ::UITEST.EXE
mcopy -i boot.img "$ROOT/gui/build/MDDESK.EXE" ::MDDESK.EXE
mcopy -i boot.img bg.bat ::BG.BAT
mcopy -i boot.img disktest.exe ::DISKTEST.EXE

echo "== building the hard disks (AHCI, NVMe)"
# make_disk image sectors label: MBR with one FAT16 partition from 1MB on,
# HELLO.TXT written by Linux
make_disk() {
  dd if=/dev/zero of="$1" bs=512 count="$2" status=none
  python3 - "$1" "$2" <<'PY'
import struct, sys
path, total = sys.argv[1], int(sys.argv[2])
entry = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x06, b"\xfe\xff\xff", 2048, total - 2048)
with open(path, "r+b") as f:
    f.seek(0x1be); f.write(entry)
    f.seek(0x1fe); f.write(b"\x55\xaa")
PY
  mformat -i "$1@@1M" -T $(($2 - 2048)) -h 255 -s 63 -H 2048 -v "$3" ::
  mcopy -i "$1@@1M" hello.txt ::HELLO.TXT
}
printf 'hello from linux\n' > hello.txt
make_disk hd.img 131072 AHCIDISK
make_disk nvme.img 196608 NVMEDISK
printf 'hello from linux\n' > linux.txt
mcopy -i boot.img linux.txt "::Created On Linux.txt"     # LFNTEST fixture

echo "== building the EFI system partition"
dd if=/dev/zero of=esp.img bs=1M count=32 status=none
mformat -i esp.img -h 64 -s 32 -t 32 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MODERNDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img boot.img ::/MODERNDOS/BOOT.IMG
printf 'hard disks writable\r\n' > diskrw
mcopy -i esp.img diskrw ::/MODERNDOS/DISKRW    # else disks are read-only

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
deadline = time.time() + 290
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
    def type_text(t):
        for c in t:
            s.sendall(("sendkey %s\n" % key(c)).encode())
            time.sleep(0.15)
    type_text("ECHO Typed: aB3!~\nECHO R")
    s.sendall(b"sendkey x 1500\n")    # held: typematic repeat
    time.sleep(2)
    type_text("\nA:\\POWEROFF\n")
    time.sleep(1); s.close()
except OSError:
    pass
PY
keys_pid=$!
python3 "$ROOT/tests/gui/gfxshot.py" debug.log mon.sock gfx.ppm &
shot_gfx=$!
python3 "$ROOT/tests/gui/mousedrive.py" debug.log mon.sock mouse.ppm &
mouse_drv=$!
(cd "$ROOT/tests/gui" && PYTHONDONTWRITEBYTECODE=1 exec python3 wmdrive.py "$WORK/debug.log" "$WORK/mon.sock" "$WORK/wm.ppm") &
wm_drv=$!
(cd "$ROOT/tests/gui" && PYTHONDONTWRITEBYTECODE=1 exec python3 uidrive.py "$WORK/debug.log" "$WORK/mon.sock" "$WORK/ui.ppm") &
ui_drv=$!
(cd "$ROOT/tests/gui" && PYTHONDONTWRITEBYTECODE=1 exec python3 deskdrive.py "$WORK/debug.log" "$WORK/mon.sock" "$WORK/desk.ppm") &
desk_drv=$!
timeout --foreground 300 qemu-system-x86_64 -m 256 -bios "$OVMF" \
  -drive file=esp.img,format=raw,if=ide -net none -display none \
  -device ahci,id=ahci -drive id=hd,file=hd.img,format=raw,if=none \
  -device ide-hd,drive=hd,bus=ahci.0 \
  -drive id=nv,file=nvme.img,format=raw,if=none \
  -device nvme,serial=MDNVME01,drive=nv \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 \
  -debugcon file:debug.log -serial file:serial.log -no-reboot \
  -monitor unix:mon.sock,server,nowait $QEMU_EXTRA </dev/null || true
kill $shot_gfx $mouse_drv $wm_drv $ui_drv $desk_drv 2>/dev/null || true
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
              "SESSIONS: 0 failures" "GFXTEST: text mode again" "ahci: port 0: QEMU HARDDISK" \
              "bios: hard disks are writable" \
              "nvme: namespace 1: 96 MB" "DISKTEST: 0 failures" \
              "disk test on D:" "xhci: port [0-9]*: keyboard" "xhci: port [0-9]*: mouse" "mouse: first report" "bios: mouse driver installed" \
              "usbkbd: first key report"; do
  if grep -q "$marker" debug.log; then echo "  ok   log: $marker"; else
    echo "  FAIL log missing: $marker"; fail=1; fi
done
for marker in "modernDOS kernel" "Hello from DOS on UEFI" "UEFI BOOT: success" \
              "^Typed: aB3!~$" "^Rxxxxx"; do
  if sed 's/\r//g' serial.log | grep -aq "$marker"; then echo "  ok   screen: $marker"; else
    echo "  FAIL screen missing: $marker"; fail=1; fi
done
# the hard disks, from Linux: check_disk image drive name
check_disk() {
  dd if="$1" of=part.img bs=512 skip=2048 status=none
  if fsck.fat -n part.img >/dev/null; then echo "  ok   $3: file system clean"; else
    echo "  FAIL $3: fsck.fat"; fail=1; fi
  if mtype -i part.img ::FROMDOS.TXT 2>/dev/null | grep -q "written by DOS on $2:"; then
    echo "  ok   $3: FROMDOS.TXT"; else echo "  FAIL $3: FROMDOS.TXT"; fail=1; fi
  rm -f big.bin
  mcopy -n -i part.img ::DISKTEST/BIG.BIN big.bin 2>/dev/null || true
  if python3 - big.bin <<'PY'
import sys
try:
    d = open(sys.argv[1], "rb").read()
except OSError:
    sys.exit(1)
want = bytes(((o * 7 + (o >> 9) + (o >> 17)) & 0xff) for o in range(1 << 20))
sys.exit(0 if d == want else 1)
PY
  then echo "  ok   $3: BIG.BIN pattern (1 MB)"; else
    echo "  FAIL $3: BIG.BIN"; fail=1; fi
}
check_disk hd.img C "AHCI disk"
check_disk nvme.img D "NVMe disk"
python3 "$ROOT/tests/gui/gfxcheck.py" gfx.ppm debug.log || fail=1
python3 "$ROOT/tests/gui/mousecheck.py" debug.log mouse.ppm || fail=1
python3 "$ROOT/tests/gui/wmcheck.py" debug.log wm.ppm || fail=1
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/gui/uicheck.py" debug.log ui.ppm || fail=1
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/gui/deskcheck.py" debug.log desk.ppm || fail=1
[ -n "$SHOTS" ] && cp ./*.ppm "$SHOTS"/ 2>/dev/null || true    # keep the screenshots
if [ $fail = 0 ]; then echo "UEFI TEST: success"; else echo "UEFI TEST: failure"; exit 1; fi
