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
for p in v86chk poweroff xmstest; do
  "$NASM" -f bin -o "$WORK/$p.com" "$HERE/$p.asm"
done
ia16-elf-gcc -mcmodel=small -Os -o "$WORK/lfntest.exe" "$ROOT/tests/lfn/lfntest.c" -li86
DJGPP=/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc
"$DJGPP" -O2 -o "$WORK/djtest.exe" "$ROOT/tests/lfn/djtest.c"
"$DJGPP" -O2 -o "$WORK/dpmitest.exe" "$HERE/dpmitest.c"

echo "== building boot floppy"
cd "$WORK"
printf 'failure\r\n' > result.txt
printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:512 /P\r\n' > fdconfig.sys
cat > autoexec.bat <<'EOF'
@ECHO OFF
ECHO failure>A:\RESULT.TXT
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
ECHO success>A:\RESULT.TXT
:END
A:\POWEROFF.COM
EOF
sed -i 's/$/\r/' autoexec.bat

"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys,result.txt" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=1 -D_SPI=2880 \
  -D_SPF=9 -D_NUMROOT=224 -D_UNIT=00h -o disk.img

export MTOOLS_SKIP_CHECK=1
mcopy -i disk.img /opt/fdos/bin/command.com ::COMMAND.COM
mcopy -i disk.img /opt/fdos/bin/himemx.exe ::HIMEMX.EXE
mcopy -i disk.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i disk.img "$ROOT/mdvmm/build/MDVMM.COM" ::MDVMM.COM
mcopy -i disk.img v86chk.com ::V86CHK.COM
mcopy -i disk.img xmstest.com ::XMSTEST.COM
mcopy -i disk.img poweroff.com ::POWEROFF.COM
mcopy -i disk.img lfntest.exe ::LFNTEST.EXE
mcopy -i disk.img djtest.exe ::DJTEST.EXE        # note: no CWSDPMI on this disk
mcopy -i disk.img dpmitest.exe ::DPMITEST.EXE
printf 'hello from linux\n' > linux.txt
mcopy -i disk.img linux.txt "::Created On Linux.txt"

echo "== booting in QEMU"
timeout --foreground 180 "$QEMU" -m 64 \
  -drive file=disk.img,format=raw,if=floppy -display none \
  -debugcon file:debug.log -no-reboot $QEMU_EXTRA </dev/null || true
sed 's/\r$//' debug.log

echo "== checks"
fail=0
result=$(mtype -i disk.img ::RESULT.TXT 2>/dev/null | tr -d '\r\n' || true)
[ "$result" = success ] || { echo "  FAIL RESULT.TXT says '$result'"; fail=1; }
for marker in "MDVMM core:" "v86: entering VM1" "v86chk: protected mode bit set" \
              "xmstest: 4K round trip" "RESULT: PASSED" "dpmi: 32 bit client" \
              "DJGPP: 0 failures" "DPMI: 0 failures"; do
  if grep -q "$marker" debug.log; then echo "  ok   log: $marker"; else
    echo "  FAIL log missing: $marker"; fail=1; fi
done
if grep -q "MDVMM PANIC" debug.log; then echo "  FAIL monitor panic"; fail=1; fi
if fsck.fat -n disk.img >/dev/null; then echo "  ok   file system clean"; else
  echo "  FAIL fsck.fat"; fail=1; fi

if [ $fail = 0 ]; then echo "MDVMM TEST: success"; else echo "MDVMM TEST: failure"; fi
exit $fail
