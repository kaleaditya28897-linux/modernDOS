#!/bin/bash
# Network test (runs inside the build container).
#
# One boot floppy, booted twice: from DOS (SeaBIOS) and from UEFI (OVMF,
# BOOTX64.EFI). QEMU gives an e1000 on its user network (DHCP, gateway
# 10.0.2.2 = the container). AUTOEXEC.BAT loads HIMEMX, MDVMM and PKTDRV
# (the packet driver on MDVMM's e1000 driver), then runs
#   NETCHK   - ARP through the packet driver (no TCP/IP stack)
#   NETTEST  - Watt-32 (DJGPP): DHCP, then a TCP connection to a server
#              the harness runs here, a line out and a line back
#
# Usage: tests/net/run.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TEST="$ROOT/test"
. "$TEST/cfg.sh"
OVMF=/usr/share/ovmf/OVMF.fd
PORT=2323
export MTOOLS_SKIP_CHECK=1

echo "== building"
make -s -C "$ROOT/mdvmm"
for p in netchk poweroff; do
  "$NASM" -f bin -o "$WORK/$p.com" "$ROOT/tests/mdvmm/$p.asm"
done
/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc -O2 -s -I/opt/watt32/inc -o "$WORK/nettest.exe" \
  "$HERE/nettest.c" -L/opt/watt32/lib -lwatt

cd "$WORK"
printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\r\n' > fdconfig.sys
cat > autoexec.bat <<EOF
@ECHO OFF
SET WATTCP.CFG=A:\\
A:\\MDVMM.COM
IF ERRORLEVEL 1 GOTO END
A:\\PKTDRV.COM
IF ERRORLEVEL 1 GOTO END
A:\\NETCHK.COM
IF ERRORLEVEL 1 GOTO END
A:\\NETTEST.EXE 10.0.2.2 $PORT
:END
A:\\POWEROFF.COM
EOF
sed -i 's/$/\r/' autoexec.bat
printf 'my_ip = dhcp\r\n' > wattcp.cfg

"$NASM" "$TEST/ldosboot/boot.asm" -w-user -I"$TEST/lmacros/" \
  -D_COMPAT_FREEDOS=1 -D_LBA=0 -D_USE_PART_INFO=0 -D_QUERY_GEOMETRY=0 \
  -D_FAT12 -D_UNIT=00h -o boot.bin
"$NASM" "$TEST/bootimg/bootimg.asm" -I ./ -I"$TEST/bootimg/" -I"$TEST/lmacros/" \
  -D_PAYLOADFILE="::rename,'$KERNEL',KERNEL.SYS,fdconfig.sys" \
  -D_BOOTPATCHFILE="'boot.bin'" \
  -D_WARN_DEFAULT_OFF=1 -D_WARN_TOOMANYFAT=0 -D_WARN_ALIGNDATA=0 \
  -D_MBR_PART_TYPE=ptFAT12 -D_BPE=12 -D_SPC=1 -D_SPI=2880 \
  -D_SPF=9 -D_NUMROOT=224 -D_UNIT=00h -o disk.img
mcopy -i disk.img /opt/fdos/bin/command.com ::COMMAND.COM
mcopy -i disk.img /opt/fdos/bin/himemx.exe ::HIMEMX.EXE
mcopy -i disk.img autoexec.bat ::AUTOEXEC.BAT
mcopy -i disk.img wattcp.cfg ::WATTCP.CFG
mcopy -i disk.img "$ROOT/mdvmm/build/MDVMM.COM" ::MDVMM.COM
mcopy -i disk.img "$ROOT/mdvmm/build/PKTDRV.COM" ::PKTDRV.COM
mcopy -i disk.img netchk.com ::NETCHK.COM
mcopy -i disk.img nettest.exe ::NETTEST.EXE
mcopy -i disk.img poweroff.com ::POWEROFF.COM

# the UEFI boot: the same floppy as \MODERNDOS\BOOT.IMG on an ESP
dd if=/dev/zero of=esp.img bs=1M count=16 status=none
mformat -i esp.img -h 64 -s 32 -t 16 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MODERNDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img disk.img ::/MODERNDOS/BOOT.IMG

# the server NETTEST talks to (10.0.2.2:PORT from inside QEMU)
python3 - $PORT <<'PY' &
import socket, sys
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[1])))
srv.listen(4)
srv.settimeout(600)
while True:
    try:
        c, _ = srv.accept()
    except OSError:
        break
    c.settimeout(20)
    data = b""
    try:
        while b"\n" not in data:
            d = c.recv(256)
            if not d:
                break
            data += d
        c.sendall(b"hello from the host, you said: " + data.strip() + b"\n")
    except OSError:
        pass
    c.close()
PY
server=$!

fail=0
run() {   # run name -- qemu args
  local name=$1; shift
  rm -f debug.log
  timeout --foreground 240 "$@" -m 128 -display none -no-reboot \
    -nic user,model=e1000 -debugcon file:debug.log </dev/null || true
  echo "-- $name"
  grep -a -E "nic:|e1000:|Packet driver|netchk:|NETTEST:|PANIC" debug.log | sed 's/\r$//'
  for marker in "nic: Intel e1000" "Packet driver at INT 60h" "netchk: ARP reply from 10.0.2.2" \
                "NETTEST: my address 10.0.2.15" "NETTEST: connected to 10.0.2.2" \
                "NETTEST: reply: hello from the host, you said: hello from modernDOS"; do
    if grep -aq "$marker" debug.log; then echo "  ok   $name: $marker"; else
      echo "  FAIL $name: missing '$marker'"; fail=1; fi
  done
}

echo "== DOS boot"
run "DOS boot" "$QEMU" -drive file=disk.img,format=raw,if=floppy
echo "== UEFI boot"
run "UEFI boot" qemu-system-x86_64 -bios "$OVMF" -drive file=esp.img,format=raw,if=ide

kill $server 2>/dev/null || true
if [ $fail = 0 ]; then echo "NET TEST: success"; else echo "NET TEST: failure"; exit 1; fi
