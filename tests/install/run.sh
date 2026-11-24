#!/bin/bash
# Install and remote update test (runs inside the build container).
#
# A machine with a GPT system disk (EFI system partition + another
# system's partition) and a personal modernDOS stick:
#   1. boot the stick, answer Y to the loader's install offer
#   2. boot from the disk alone: C: is the EFI system partition, the SSH
#      keys are kept there, SSHD starts by itself
#   3. over SSH: put a build into C:\MDOS\TRY, TRYBOOT; the trial
#      runs; nobody confirms it: the machine restarts into the installed
#      build by itself
#   4. TRYBOOT again, KEEP: the trial becomes the installed build
# The other system's partition must stay as it was, and the host key the
# same through all restarts.
#
# Usage: tests/install/run.sh [kernel.sys]

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=$(mktemp -d)
ISO="$ROOT/_output/release/modernDOS-0.0.0-installtest.iso"
trap 'kill $QPID 2>/dev/null || true; rm -rf "$WORK" "$ISO" "$ISO.sha256"' EXIT
OVMF=/usr/share/ovmf/OVMF.fd
PORT=2222
export MTOOLS_SKIP_CHECK=1

echo "== building a personal stick image"
cd "$WORK"
mkdir personal
ssh-keygen -q -t ed25519 -N "" -C tester -f id
sed 's/$/\r/' id.pub > personal/AUTHKEYS.TXT
head -c 32 /dev/urandom > personal/HOSTKEY.ED
PERSONAL="$WORK/personal" "$ROOT/release/build.sh" 0.0.0-installtest > build.log 2>&1 || { tail -20 build.log; exit 1; }
cp "$ISO" stick.img
truncate -s 128M stick.img
ESP=$(python3 "$HERE/mkgpt.py" disk.img 512 200)
mformat -i disk.img@@$ESP -F -T $((200 * 2048)) -h 255 -s 63 -v EFI ::
cp "$ROOT/mdvmm/build/BOOTX64.EFI" new.efi
mcopy -i "$ISO@@$(python3 - "$ISO" <<'PY'
import struct, sys
f = open(sys.argv[1], "rb"); f.seek(512); h = f.read(92)
lba, n, size = struct.unpack_from("<QII", h, 72)
for i in range(n):
    f.seek(lba * 512 + i * size); e = f.read(size)
    if e[:16] == bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b"):
        print(struct.unpack_from("<Q", e, 32)[0] * 512)
PY
)" ::/MDOS/BOOT.IMG new.img

fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
check() { local what=$1; shift; if "$@" > out.txt 2>&1; then ok "$what"; else bad "$what"; sed 's/^/       /' out.txt | head -12; fi; }
DISK="-device ahci,id=ahci -drive id=hd,file=disk.img,format=raw,if=none -device ide-hd,drive=hd,bus=ahci.0,bootindex=1"

echo "== 1. install from the stick"
qemu-system-x86_64 -m 256 -bios "$OVMF" -display none -net none $DISK \
  -drive if=none,id=stick,format=raw,file=stick.img -device qemu-xhci -device usb-storage,drive=stick,bootindex=0 \
  -serial file:serial.log -debugcon file:debug.log -monitor unix:mon.sock,server,nowait </dev/null &
QPID=$!
python3 - <<'PY'
import socket, time
def text():
    try: return open("serial.log", "rb").read()
    except OSError: return b""
end = time.time() + 90
while time.time() < end and b"Press Y to install" not in text():
    time.sleep(0.5)
time.sleep(1)
s = socket.socket(socket.AF_UNIX); s.connect("mon.sock"); s.sendall(b"sendkey y\n"); time.sleep(0.5); s.close()
while time.time() < end and b"Starting from the stick" not in text() and b"Install: FAILED" not in text():
    time.sleep(0.5)
PY
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
sed 's/\r//g; s/\x1b\[[0-9;=?]*[A-Za-z]//g' serial.log | grep -a "INSTALL\|copied\|FAILED\|Installed\|Install:" | sed 's/^/   /'
check "the loader reports: Installed" grep -aq "^Installed" serial.log
check "BOOTX64.EFI on the disk's EFI partition" bash -c "mcopy -i disk.img@@$ESP ::/EFI/BOOT/BOOTX64.EFI a.efi && cmp a.efi $ROOT/mdvmm/build/BOOTX64.EFI"
check "BOOT.IMG and DISKRW on the disk" bash -c "mdir -i disk.img@@$ESP ::/MDOS | grep -q 'BOOT *IMG' && mdir -i disk.img@@$ESP ::/MDOS | grep -q DISKRW"
check "the other system's partition is untouched" bash -c "dd if=disk.img bs=512 skip=$((2048 + 200 * 2048)) count=1 status=none | grep -aq OTHER-OS-DATAOTHER-OS-DATA"

echo "== 2. boot from the disk"
OPTS=(-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$WORK/known" -o LogLevel=ERROR
      -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=15 -o ServerAliveInterval=5
      -o ServerAliveCountMax=2 -i "$WORK/id" -p $PORT)
rm -f debug.log
qemu-system-x86_64 -m 256 -bios "$OVMF" -display none $DISK \
  -nic user,model=e1000,hostfwd=tcp:127.0.0.1:$PORT-:22 -debugcon file:debug.log \
  -serial file:serial2.log </dev/null &
QPID=$!
boots() { local n; n=$(grep -ac "sshd: listening" debug.log 2>/dev/null) || true; echo "${n:-0}"; }
wait_boot() {   # wait_boot n seconds: the n-th "listening"
  local i
  for i in $(seq 1 "$2"); do [ "$(boots)" -ge "$1" ] && return 0; sleep 1; done
  return 1
}
dos() { timeout 60 ssh "${OPTS[@]}" dos@127.0.0.1 "$@" 2>&1 | tr -d '\r'; }
# dosgrep "what" "pattern" command: its output must have the pattern
dosgrep() {
  local what=$1 pat=$2; shift 2
  dos "$@" > out.txt || true
  if grep -q "$pat" out.txt; then ok "$what"; else bad "$what"; sed 's/^/       /' out.txt | head -14; fi
}

check "booted from the disk, SSHD listening" wait_boot 1 120
[ "$(boots)" -ge 1 ] || { sed 's/\r//g; s/\x1b\[[0-9;=?]*[A-Za-z]//g' serial2.log | tr -d '\000' | grep -av "^$" | grep -av "Press ESC" | head -30; sed 's/\r$//' debug.log | tail -25; [ -n "$KEEP" ] && cp debug.log "$KEEP/install-debug.log"; echo "INSTALL TEST: failure"; exit 1; }
check "C: is the EFI system partition (GPT)" grep -aq "GPT partition 1: FAT32" debug.log
dosgrep "the SSH keys are kept on C:" 'HOSTKEY  ED' 'DIR C:\MDOS\SSH'
dos 'COPY A:\WATTCP.CFG C:\MDOS\NOTE.TXT' > /dev/null
dosgrep "MDSYS: installed system" 'Installed system' MDSYS

if [ -n "$DIAG" ]; then   # DIAG=1: show the disk as DOS sees it, stop here
  for c in 'DIR A:\MDSYS.COM' 'DIR C:\' 'DIR C:\MDOS /B' 'IF EXIST C:\MDOS\SSH\HOSTKEY.ED ECHO key-file-exists' 'TYPE C:\MDOS\SSH\WATTCP.CFG' 'VOL C:' 'CHKDSK C:'; do
    echo "--- $c"; dos "$c" | head -12
  done
  printf 'ls -l /C/MDOS/SSH\nls -l /C/MDOS\n' > b2; echo "--- sftp ls"; sftp -b b2 -P $PORT "${OPTS[@]:0:16}" -i "$WORK/id" dos@127.0.0.1 2>&1 | head -20
  echo "--- monitor"; grep -a "bios:.*GPT\|bios: hard" debug.log | sed 's/\r//'
  kill $QPID 2>/dev/null || true; exit 1
fi
echo "== 3. a trial that nobody confirms"
: > fast
printf 'mkdir /C/MDOS/TRY\nput new.efi /C/MDOS/TRY/BOOTX64.EFI\nput new.img /C/MDOS/TRY/BOOT.IMG\nput fast /C/MDOS/TRY/FAST\n' > batch
check "build uploaded to C:\\MDOS\\TRY (sftp)" sftp -b batch -P $PORT "${OPTS[@]:0:16}" -i "$WORK/id" dos@127.0.0.1
dos TRYBOOT > /dev/null || true
check "restarted into the trial" wait_boot 2 120
dosgrep "MDSYS: trial boot" 'TRIAL boot' MDSYS
check "not confirmed: restarted by itself" wait_boot 3 180
check "the monitor says why" grep -aq "trial boot: not confirmed in time, restarting" debug.log
dosgrep "MDSYS: the installed system again" 'Installed system' MDSYS

echo "== 4. a trial that is kept"
dos TRYBOOT > /dev/null || true
check "restarted into the trial" wait_boot 4 120
dosgrep "KEEP makes it the installed build" 'installed one now' KEEP
sleep 75
check "confirmed: no restart after the trial time" test "$(boots)" = 4
dos 'MDSYS REBOOT' > /dev/null || true
check "restarted on request" wait_boot 5 120
dosgrep "MDSYS: installed system" 'Installed system' MDSYS
dosgrep "a file written to C: before is still there" 'dhcp' 'TYPE C:\MDOS\NOTE.TXT'
check "one host key through all restarts" test "$(grep -c . known)" = 1

kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
[ -n "$KEEP" ] && cp debug.log "$KEEP/install-debug.log"
echo "   monitor log (trial lines):"; grep -a "trial\|TRIAL\|restart" debug.log | sed 's/\r$//; s/^/     /' | head -12
if [ $fail = 0 ]; then echo "INSTALL TEST: success"; exit 0; else echo "INSTALL TEST: failure"; exit 1; fi
