#!/bin/bash
# Compatibility test with existing DOS software (runs inside the build
# container): programs nobody here wrote, of the kinds DOS software comes
# in, on the BIOS edition's disk (started by SeaBIOS) under the monitor:
#
#   - a 16-bit real mode program (Open Watcom, large model)
#   - 32-bit programs under the DOS extenders of the DOS years: DOS/4GW,
#     PMODE/W, DOS/32A, CauseWay (each finds the monitor's DPMI host, or
#     must cope with running in V86 mode without VCPI)
#   - the Open Watcom C compiler itself, hosted on DOS: WCL386 compiles and
#     links a program on modernDOS (a DOS/4GW program that runs the
#     compiler and the linker: DPMI clients started by a DPMI client)
#   - FreeDOS FDISK, FORMAT, XCOPY and the kernel's SYS run in tests/bios
#
# The programs write to C:\OUT.TXT, which is read from the disk image
# afterwards.
#
# Usage: tests/compat/run.sh

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=$(mktemp -d)
VER=0.0.0-compattest
OUT="$ROOT/_output/release"
trap 'rm -rf "$WORK" "$OUT/modernDOS-$VER"*' EXIT
export MTOOLS_SKIP_CHECK=1
export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binw:$PATH    # binw: the extenders' stubs for the linker
export INCLUDE=$WATCOM/h
. "$ROOT/test/cfg.sh"

echo "== building"
cd "$WORK"
"$ROOT/release/build.sh" $VER > build.log 2>&1 || { tail -20 build.log; exit 1; }
cp "$OUT/modernDOS-$VER-bios.img" disk.img
PART=$((1008 * 512))
"$NASM" -f bin -o poweroff.com "$ROOT/tests/mdvmm/poweroff.asm"
cp "$HERE/hello.c" hello.c
# the same program for every kind
wcl -q -bt=dos -ml -fe=hello16.exe hello.c > w16.log 2>&1 || { cat w16.log; exit 1; }
for ext in dos4g pmodew dos32a causeway; do
  wcl386 -q -bt=dos -l=$ext -fe=h_$ext.exe hello.c > w_$ext.log 2>&1 || { cat w_$ext.log; exit 1; }
done
# VGA mode 13h, in real mode and under DOS/4GW
wcl -q -bt=dos -ml -fe=vga16.exe "$HERE/vga.c" > v16.log 2>&1 || { cat v16.log; exit 1; }
wcl386 -q -bt=dos -l=dos4g -fe=vga32.exe "$HERE/vga.c" > v32.log 2>&1 || { cat v32.log; exit 1; }
ls -l hello16.exe h_*.exe vga*.exe | awk '{print "   " $5, $9}'

# the DOS-hosted compiler: what WCL386 needs to compile and link for DOS/4GW
mmd -i disk.img@@$PART ::/WATCOM ::/WATCOM/BINW ::/WATCOM/H ::/WATCOM/LIB386 ::/WATCOM/LIB386/DOS ::/T
for f in wcl386.exe wcc386.exe wlink.exe wlink.lnk dos4gw.exe wstub.exe wcc38601.int; do
  mcopy -i disk.img@@$PART $WATCOM/binw/$f ::/WATCOM/BINW/
done
for f in stdio.h stdlib.h string.h dos.h i86.h _comdef.h malloc.h; do
  [ -f $WATCOM/h/$f ] && mcopy -i disk.img@@$PART $WATCOM/h/$f ::/WATCOM/H/
done
mcopy -i disk.img@@$PART $WATCOM/lib386/dos/clib3r.lib $WATCOM/lib386/dos/emu387.lib ::/WATCOM/LIB386/DOS/
mcopy -i disk.img@@$PART $WATCOM/lib386/math387r.lib ::/WATCOM/LIB386/
mcopy -i disk.img@@$PART $WATCOM/binw/dos4gw.exe $WATCOM/binw/pmodew.exe $WATCOM/binw/dos32a.exe ::/T/
mcopy -i disk.img@@$PART hello16.exe h_dos4g.exe h_pmodew.exe h_dos32a.exe h_causeway.exe hello.c poweroff.com \
  vga16.exe vga32.exe ::/T/

# AUTOEXEC.BAT: the edition's own, then the programs
mtype -i disk.img@@$PART ::AUTOEXEC.BAT > autoexec.bat
printf '%s\r\n' 'SET WATCOM=C:\WATCOM' 'SET INCLUDE=C:\WATCOM\H' 'PATH C:\MDOS;C:\;C:\WATCOM\BINW' \
  'C:' 'CD \T' ${COMPAT_FIRST:+"$COMPAT_FIRST"} \
  'ECHO === real mode >C:\OUT.TXT' 'HELLO16.EXE >>C:\OUT.TXT' \
  'ECHO === DOS/4GW >>C:\OUT.TXT' 'H_DOS4G.EXE >>C:\OUT.TXT' \
  'ECHO === PMODE/W >>C:\OUT.TXT' 'H_PMODEW.EXE >>C:\OUT.TXT' \
  'ECHO === DOS/4GW again >>C:\OUT.TXT' 'H_DOS4G.EXE >>C:\OUT.TXT' \
  'ECHO === compiled here >>C:\OUT.TXT' 'COPY HELLO.C MADE.C >NUL' \
  'WCL386 -q -bt=dos -l=dos4g MADE.C >C:\WCL.TXT' 'MADE.EXE >>C:\OUT.TXT' \
  'ECHO === DOS/32A >>C:\OUT.TXT' 'H_DOS32A.EXE >>C:\OUT.TXT' \
  'ECHO === CauseWay >>C:\OUT.TXT' 'H_CAUSEW.EXE >>C:\OUT.TXT' \
  'ECHO === VGA 16 >>C:\OUT.TXT' 'VGA16.EXE >>C:\OUT.TXT' \
  'ECHO === VGA 32 >>C:\OUT.TXT' 'VGA32.EXE >>C:\OUT.TXT' \
  'ECHO === end >>C:\OUT.TXT' 'POWEROFF.COM' >> autoexec.bat
mdel -i disk.img@@$PART ::AUTOEXEC.BAT
mcopy -i disk.img@@$PART autoexec.bat ::AUTOEXEC.BAT
mren -i disk.img@@$PART ::/T/h_causeway.exe ::/T/H_CAUSEW.EXE 2>/dev/null || true

echo "== booting"
qemu-system-i386 -m 128 -display none -drive file=disk.img,format=raw,if=ide \
  -nic user,model=e1000 -debugcon file:debug.log -no-reboot -monitor unix:mon.sock,server,nowait </dev/null &
QPID=$!
python3 "$HERE/vgawatch.py" debug.log mon.sock "$WORK/vga" &
for i in $(seq 1 ${COMPAT_TIME:-300}); do kill -0 $QPID 2>/dev/null || break; sleep 1; done
if kill -0 $QPID 2>/dev/null; then    # still there: what is on its screen?
  python3 -c 'import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.connect("mon.sock"); s.sendall(("screendump %s\n" % sys.argv[1]).encode()); time.sleep(1)' \
    "$ROOT/_output/compat-hang.ppm" 2>/dev/null || true
  python3 -c 'import socket,time
s=socket.socket(socket.AF_UNIX); s.connect("mon.sock"); time.sleep(0.3); s.recv(65536)
for i in range(4):
    s.sendall(b"info registers\n"); time.sleep(0.4)
    d=s.recv(65536).decode("latin1")
    print("   cpu: "+" ".join(x for x in d.split() if x.startswith(("EIP=","CS","SS","EFL=","ESP=","EAX="))))' 2>/dev/null || true
  kill $QPID 2>/dev/null || true
fi
wait $QPID 2>/dev/null || true
mtype -i disk.img@@$PART ::OUT.TXT 2>/dev/null | tr -d '\r' > out.txt || true
mtype -i disk.img@@$PART ::WCL.TXT 2>/dev/null | tr -d '\r' > wcl.txt || true
sed 's/^/   /' out.txt
[ -s wcl.txt ] && sed 's/^/   wcl386: /' wcl.txt | head -12
grep -a "^v86: \|^dpmi: \|PANIC" -A6 debug.log | grep -av "^--\|^vm: \|^nic: " | head -40 | sed "s/^/   monitor: /" || true

fail=0
section() { sed -n "/^=== $1 *\$/,/^=== [^ ]* *[^ ]* *\$/p" out.txt; }
for kind in "real mode" "DOS\\/4GW" "PMODE\\/W" "DOS\\/4GW again" "compiled here" "DOS\\/32A" "CauseWay"; do
  name=$(echo "$kind" | tr -d '\\')
  if section "$kind" | grep -q "hello: all 5 ok"; then echo "  ok   $name"; else echo "  FAIL $name"; fail=1; fi
done
for bits in 16 32; do
  if section "VGA $bits" | grep -q "vga: $bits bit, mode 13h, palette 63/0/0: all ok" &&
     python3 "$HERE/vgacheck.py" vga$bits.ppm; then
    echo "  ok   VGA mode 13h, $bits bit: palette, pixels, the picture on the display"
  else echo "  FAIL VGA mode 13h, $bits bit"; fail=1; fi
done
grep -q "^=== end" out.txt && echo "  ok   ran to the end" || { echo "  FAIL did not run to the end"; fail=1; }
if [ $fail = 0 ]; then echo "COMPAT TEST: success"; exit 0; else echo "COMPAT TEST: failure"; exit 1; fi
