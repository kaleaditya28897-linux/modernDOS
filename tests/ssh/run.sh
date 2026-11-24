#!/bin/bash
# SSH server test on modernDOS (runs inside the build container).
#
# A boot floppy with MDVMM, SSHSLOT (job slots), PKTDRV and SSHD, booted
# from DOS (SeaBIOS) and from UEFI (OVMF). QEMU's user network forwards
# 127.0.0.1:2222 to the guest's port 22; OpenSSH here then checks:
#   exec (ECHO), an interactive shell (ANSI screen), sftp up and down,
#   the host key fingerprint, and finally powers the guest off over SSH.
#
# Usage: tests/ssh/run.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KERNEL=$(realpath "${1:-$ROOT/_output/gcc/kgc8632.sys}")
WORK=$(mktemp -d)
trap 'kill $QPID 2>/dev/null || true; rm -rf "$WORK"' EXIT
TEST="$ROOT/test"
. "$TEST/cfg.sh"
OVMF=/usr/share/ovmf/OVMF.fd
PORT=2222
export MTOOLS_SKIP_CHECK=1

echo "== building"
make -s -C "$ROOT/mdvmm"
make -s -C "$ROOT/net/ssh"
"$NASM" -f bin -o "$WORK/poweroff.com" "$ROOT/tests/mdvmm/poweroff.asm"

cd "$WORK"
ssh-keygen -q -t ed25519 -N "" -C tester -f id
printf 'DEVICE=A:\\HIMEMX.EXE\r\nDOS=HIGH\r\nSHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\r\n' > fdconfig.sys
cat > autoexec.bat <<EOF
@ECHO OFF
SET WATTCP.CFG=A:\\
SET TEMP=A:\\
A:\\MDVMM.COM
IF ERRORLEVEL 1 GOTO END
A:\\SSHSLOT.COM 2
IF ERRORLEVEL 1 GOTO END
A:\\PKTDRV.COM
IF ERRORLEVEL 1 GOTO END
A:\\SSHD.EXE
:END
A:\\POWEROFF.COM
EOF
sed -i 's/$/\r/' autoexec.bat
printf 'my_ip = dhcp\r\n' > wattcp.cfg
{ echo "# keys allowed to log in"; cat id.pub; } | sed 's/$/\r/' > authkeys.txt

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
mcopy -i disk.img authkeys.txt ::AUTHKEYS.TXT
mcopy -i disk.img "$ROOT/mdvmm/build/MDVMM.COM" ::MDVMM.COM
mcopy -i disk.img "$ROOT/mdvmm/build/PKTDRV.COM" ::PKTDRV.COM
mcopy -i disk.img "$ROOT/mdvmm/build/SSHSLOT.COM" ::SSHSLOT.COM
mcopy -i disk.img "$ROOT/net/ssh/build/sshd.exe" ::SSHD.EXE
mcopy -i disk.img /opt/cwsdpmi/bin/CWSDPMI.EXE ::CWSDPMI.EXE
mcopy -i disk.img poweroff.com ::POWEROFF.COM
echo "floppy: $(mdir -i disk.img -w :: | wc -w) files, $(mdu -i disk.img -s :: 2>/dev/null | tail -1)"

dd if=/dev/zero of=esp.img bs=1M count=16 status=none
mformat -i esp.img -h 64 -s 32 -t 16 ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/MODERNDOS
mcopy -i esp.img "$ROOT/mdvmm/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img disk.img ::/MODERNDOS/BOOT.IMG

OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$WORK/known" -o LogLevel=ERROR
      -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=20 -i "$WORK/id" -p $PORT)
fail=0
check() {   # check "what" command...
  local what=$1; shift
  if "$@" > out.txt 2>&1; then echo "  ok   $name: $what"; else
    echo "  FAIL $name: $what"; sed 's/^/       /' out.txt | head -20; fail=1; fi
}

run() {   # run name -- qemu args
  name=$1; shift
  rm -f debug.log known
  timeout --foreground 400 "$@" -m 128 -display none -no-reboot \
    -nic user,model=e1000,hostfwd=tcp:127.0.0.1:$PORT-:22 -debugcon file:debug.log </dev/null &
  QPID=$!
  for i in $(seq 1 120); do
    grep -aq "sshd: listening" debug.log 2>/dev/null && break
    grep -aq "PANIC" debug.log 2>/dev/null && break
    sleep 1
  done
  echo "-- $name ($i s to start)"
  grep -a "sshd: \|Packet driver\|PANIC" debug.log | sed 's/\r$//; s/^/   /'

  check "exec ECHO" bash -c "ssh ${OPTS[*]} dos@127.0.0.1 'ECHO hello from DOS' | tr -d '\r' | grep -qx 'hello from DOS'"
  check "exec DIR shows the files" bash -c "ssh ${OPTS[*]} dos@127.0.0.1 'DIR A:\\' | grep -q 'SSHD     EXE'"

  head -c 50000 /dev/urandom > blob
  printf 'put blob /A/UP.BIN\nls -l /A\nget /A/UP.BIN back\nrm /A/UP.BIN\nls /\n' > batch
  check "sftp put, ls, get, rm" sftp -b batch -P $PORT "${OPTS[@]:0:12}" -i "$WORK/id" dos@127.0.0.1
  check "sftp: file came back unchanged" cmp blob back

  # interactive shell: type a command, read it back from the screen
  python3 - "$PORT" > shell.txt 2>&1 <<'PY' &&
import subprocess, sys, time, os
p = subprocess.Popen(["ssh", "-tt", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=known",
                      "-o", "LogLevel=ERROR", "-o", "BatchMode=yes", "-i", "id", "-p", sys.argv[1],
                      "dos@127.0.0.1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
os.set_blocking(p.stdout.fileno(), False)
out = b""
def wait_for(text, secs):
    global out
    end = time.time() + secs
    while time.time() < end:
        try:
            d = p.stdout.read()
        except BlockingIOError:
            d = None
        if d:
            out += d
        if text in out:
            return True
        time.sleep(0.1)
    return False
ok = wait_for(b">", 30)
p.stdin.write(b"ECHO SHELL-%COMSPEC%\r"); p.stdin.flush()
ok = ok and wait_for(b"SHELL-A:\\COMMAND.COM", 30)
p.stdin.write(b"EXIT\r"); p.stdin.flush()
try:
    p.wait(timeout=30)
except subprocess.TimeoutExpired:
    p.kill(); ok = False
wait_for(b"\x00", 1)
sys.stdout.write(out.decode("utf-8", "replace"))
sys.exit(0 if ok else 1)
PY
  { echo "  ok   $name: shell: typed ECHO, its output came back"; } || {
    echo "  FAIL $name: shell"; sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g' shell.txt | tr -s ' ' | head -c 600; echo; fail=1; }

  fp=$(ssh-keygen -l -F "[127.0.0.1]:$PORT" -f known 2>/dev/null | awk '/ED25519/ {print $3}')
  check "host key fingerprint matches the server's log" grep -aq "$fp" debug.log

  ssh "${OPTS[@]}" dos@127.0.0.1 'A:\POWEROFF.COM' > /dev/null 2>&1 || true
  for i in $(seq 1 30); do kill -0 $QPID 2>/dev/null || break; sleep 1; done
  kill $QPID 2>/dev/null && { echo "  FAIL $name: no power off over SSH"; fail=1; } || echo "  ok   $name: powered off over SSH"
  wait $QPID 2>/dev/null || true
  echo "   server log:"
  grep -a "sshd: \|vm: session\|PANIC\|dpmi: " debug.log | sed 's/\r$//; s/^/     /' | tail -40
}

echo "== DOS boot"
run "DOS boot" "$QEMU" -drive file=disk.img,format=raw,if=floppy
echo "== UEFI boot"
run "UEFI boot" qemu-system-x86_64 -bios "$OVMF" -drive file=esp.img,format=raw,if=ide

if [ $fail = 0 ]; then echo "SSH TEST: success"; exit 0; else echo "SSH TEST: failure"; exit 1; fi
