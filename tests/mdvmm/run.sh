#!/bin/bash
# MDVMM integration test (runs inside the build container).
#
# Boots a floppy with HIMEMX (DOS=HIGH) and FreeCOM; AUTOEXEC.BAT loads
# MDVMM and then runs, inside the virtual machine:
#   V86CHK   - CPU is in V86 mode and the MDVMM API answers
#   XMSTEST  - XMS moves work (monitor emulates INT 15h AH=87h)
#   LFNTEST  - the full long filename suite (heavy INT 21h, timer, disk)
#   DJTEST   - DJGPP libc long filename test, using MDVMM as DPMI host
#   DPMITEST - DPMI host test: memory, exceptions, IRQ0, callbacks, ...
#   MDVMM NEW BG.BAT + SESSTEST /FG - a second DOS session runs a background
#              job while session 0 checks preemption, hotkeys and results
# There is no CWSDPMI on the disk: DJGPP programs must use MDVMM.
# MDVMM and the tests log to the QEMU debug console.
#
# Usage: tests/mdvmm/run.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TEST="$ROOT/test"
. "$TEST/cfg.sh"

echo "== building MDVMM and test programs"
make -s -C "$ROOT/mdvmm"
make -s -C "$ROOT/gui"
for p in v86chk poweroff xmstest netchk; do
  "$NASM" -f bin -o "$WORK/$p.com" "$HERE/$p.asm"
done
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/lfntest.exe" "$ROOT/tests/lfn/lfntest.c" -li86
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/sesstest.exe" "$HERE/sesstest.c" -li86
DJGPP=/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc
"$DJGPP" -O2 -s -o "$WORK/djtest.exe" "$ROOT/tests/lfn/djtest.c"
"$DJGPP" -O2 -s -o "$WORK/dpmitest.exe" "$HERE/dpmitest.c"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/gfxtest.exe" "$ROOT/tests/gui/gfxtest.c" "$ROOT/gui/build/mdgui.o"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/mousetest.exe" "$ROOT/tests/gui/mousetest.c" "$ROOT/gui/build/mdgui.o"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/wmtest.exe" "$ROOT/tests/gui/wmtest.c" "$ROOT/gui/build/mdgui.o"
"$DJGPP" -O2 -s -I"$ROOT/gui/lib" -o "$WORK/uitest.exe" "$ROOT/tests/gui/uitest.c" "$ROOT/gui/build/mdgui.o"

echo "== building boot floppy"
cd "$WORK"
printf 'failure\r\n' > result.txt
printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
cat > autoexec.bat <<'EOF'
@ECHO OFF
ECHO failure>A:\RESULT.TXT
A:\CTMOUSE.EXE
A:\MDVMM.COM
IF ERRORLEVEL 1 GOTO END
A:\V86CHK.COM
IF ERRORLEVEL 1 GOTO END
A:\PKTDRV.COM
IF ERRORLEVEL 1 GOTO END
A:\NETCHK.COM
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
A:\SSHSLOT.COM 2
A:\MDDESK.EXE /TEST
IF ERRORLEVEL 1 GOTO END
A:\MDDESK.EXE /TEST /EXIT
IF ERRORLEVEL 1 GOTO END
ECHO success>A:\RESULT.TXT
:END
A:\POWEROFF.COM
EOF
sed -i 's/$/\r/' autoexec.bat
printf '@ECHO OFF\r\nA:\\SESSTEST.EXE /BG\r\nECHO done>A:\\BGDONE.TXT\r\n' > bg.bat

"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys,result.txt" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=2 -D_SPI=5760 \
  -D_SPF=9 -D_NUMROOT=224 -D_CHS_SECTORS=36 -D_CHS_HEADS=2 -D_UNIT=00h -o disk.img

export MTOOLS_SKIP_CHECK=1
mcopy -i disk.img /opt/fdos/bin/command.com ::COMMAND.COM
mcopy -i disk.img /opt/fdos/bin/himemx.exe ::HIMEMX.EXE
mcopy -i disk.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i disk.img "$ROOT/mdvmm/build/MDVMM.COM" ::MDVMM.COM
mcopy -i disk.img v86chk.com ::V86CHK.COM
mcopy -i disk.img "$ROOT/mdvmm/build/PKTDRV.COM" ::PKTDRV.COM
mcopy -i disk.img netchk.com ::NETCHK.COM
mcopy -i disk.img xmstest.com ::XMSTEST.COM
mcopy -i disk.img poweroff.com ::POWEROFF.COM
mcopy -i disk.img lfntest.exe ::LFNTEST.EXE
mcopy -i disk.img djtest.exe ::DJTEST.EXE        # note: no CWSDPMI on this disk
mcopy -i disk.img dpmitest.exe ::DPMITEST.EXE
mcopy -i disk.img sesstest.exe ::SESSTEST.EXE
mcopy -i disk.img gfxtest.exe ::GFXTEST.EXE
mcopy -i disk.img mousetest.exe ::MOUSETEST.EXE
mcopy -i disk.img wmtest.exe ::WMTEST.EXE
mcopy -i disk.img uitest.exe ::UITEST.EXE
mcopy -i disk.img "$ROOT/gui/build/MDDESK.EXE" ::MDDESK.EXE
mcopy -i disk.img "$ROOT/mdvmm/build/SSHSLOT.COM" ::SSHSLOT.COM
mcopy -i disk.img /opt/fdos/bin/ctmouse.exe ::CTMOUSE.EXE
mcopy -i disk.img bg.bat ::BG.BAT
printf 'hello from linux\n' > linux.txt
mcopy -i disk.img linux.txt "::Created On Linux.txt"

echo "== booting in QEMU"
# session hotkeys: once the sessions test runs, press Ctrl+Alt+F2 and
# Ctrl+Alt+F1 alternately through the QEMU monitor
python3 - "$WORK" <<'PY' &
import os, socket, sys, time
work = sys.argv[1]
log, sock = os.path.join(work, "debug.log"), os.path.join(work, "mon.sock")
def text():
    try:
        return open(log, "rb").read()
    except OSError:
        return b""
deadline = time.time() + 290
while time.time() < deadline and b"sessions test" not in text():
    time.sleep(0.2)
keys = ["ctrl-alt-f2", "ctrl-alt-f1"]
i = 0
while time.time() < deadline and b"SESSIONS:" not in text():
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock)
        s.sendall(("sendkey %s\n" % keys[i % 2]).encode())
        time.sleep(0.2)
        s.close()
    except OSError:
        break
    i += 1
    time.sleep(1.5)
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
timeout --foreground 300 "$QEMU" -m 64 -nic user,model=e1000 \
  -drive file=disk.img,format=raw,if=floppy -display none \
  -debugcon file:debug.log -no-reboot \
  -monitor unix:mon.sock,server,nowait $QEMU_EXTRA </dev/null || true
kill $shot_gfx $mouse_drv $wm_drv $ui_drv $desk_drv 2>/dev/null || true
kill $keys_pid 2>/dev/null || true
sed 's/\r$//' debug.log

echo "== checks"
fail=0
result=$(mtype -i disk.img ::RESULT.TXT 2>/dev/null | tr -d '\r\n' || true)
[ "$result" = success ] || { echo "  FAIL RESULT.TXT says '$result'"; fail=1; }
for marker in "MDVMM core:" "v86: entering VM1" "v86chk: protected mode bit set" \
              "xmstest: 4K round trip" "RESULT: PASSED" "dpmi: 32 bit client" \
              "DJGPP: 0 failures" "DPMI: 0 failures" "vm: sessions enabled" \
              "vm: session 1 started" "vm: focus session 1" "vm: session 1 ended" \
              "SESSIONS: 0 failures" "nic: 82540em (iPXE)" "Packet driver at INT 60h" "netchk: ARP reply from 10.0.2.2" "GFXTEST: text mode again"; do
  if grep -q "$marker" debug.log; then echo "  ok   log: $marker"; else
    echo "  FAIL log missing: $marker"; fail=1; fi
done
if grep -q "MDVMM PANIC" debug.log; then echo "  FAIL monitor panic"; fail=1; fi
if fsck.fat -n disk.img >/dev/null; then echo "  ok   file system clean"; else
  echo "  FAIL fsck.fat"; fail=1; fi

python3 "$ROOT/tests/gui/gfxcheck.py" gfx.ppm debug.log || fail=1
python3 "$ROOT/tests/gui/mousecheck.py" debug.log mouse.ppm loose || fail=1   # CTMOUSE accelerates
python3 "$ROOT/tests/gui/wmcheck.py" debug.log wm.ppm || fail=1
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/gui/uicheck.py" debug.log ui.ppm || fail=1
TERMINAL=1 PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/gui/deskcheck.py" debug.log desk.ppm || fail=1
[ -n "$SHOTS" ] && cp ./*.ppm "$SHOTS"/ 2>/dev/null || true    # keep the screenshots
# the desktop's editor changed this file (DOS boot: the disk is kept)
if [ "$(mtype -i disk.img "::Created On Linux.txt" 2>/dev/null | od -An -c | tr -d ' \n')" = \
     "hellofromlinux-editedinmodernDOS\r\nsecondline\r\n" ]; then
  echo "  ok   editor's file on the disk"; else echo "  FAIL editor's file on the disk"; fail=1; fi
if [ $fail = 0 ]; then echo "MDVMM TEST: success"; else echo "MDVMM TEST: failure"; fi
exit $fail
