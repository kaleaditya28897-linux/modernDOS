#!/bin/bash
# BIOS edition test (runs inside the build container).
#
# The BIOS disk image (release/build.sh, a personal one with a test key),
# started by SeaBIOS as an IDE disk and as a USB stick, with different
# network cards. Each boot must bring up the SSH server by itself; over SSH:
#   - the system is on C: (the stick itself), files written stay
#   - a page comes over http from a server here (HTTPGET)
#   - PKG and the SSH client are there
#
# Usage: tests/bios/run.sh

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=$(mktemp -d)
VER=0.0.0-biostest
OUT="$ROOT/_output/release"
trap 'kill $QPID $WEB 2>/dev/null || true; rm -rf "$WORK" "$OUT/modernDOS-$VER"*' EXIT
PORT=2223
export MTOOLS_SKIP_CHECK=1

echo "== building a personal BIOS image"
cd "$WORK"
mkdir personal site
ssh-keygen -q -t ed25519 -N "" -C tester -f id
sed 's/$/\r/' id.pub > personal/AUTHKEYS.TXT
head -c 32 /dev/urandom > personal/HOSTKEY.ED
PERSONAL="$WORK/personal" "$ROOT/release/build.sh" $VER > build.log 2>&1 || { tail -20 build.log; exit 1; }
cp "$OUT/modernDOS-$VER-bios.img" disk.img
PART=$((1008 * 512))
mcopy -i disk.img@@$PART "$ROOT/net/pkg/build/httpget.exe" ::/MDOS/HTTPGET.EXE
echo '<title>hello</title>plain page' > site/index.html
python3 -m http.server 8379 --bind 127.0.0.1 -d site > /dev/null 2>&1 &
WEB=$!

fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
OPTS=(-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$WORK/known" -o LogLevel=ERROR
      -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=15 -o ServerAliveInterval=5
      -o ServerAliveCountMax=2 -i "$WORK/id" -p $PORT)
dos() { timeout 90 ssh "${OPTS[@]}" dos@127.0.0.1 "$@" 2>&1 | tr -d '\r'; }
dosgrep() {   # dosgrep "what" "pattern" command
  local what=$1 pat=$2; shift 2
  dos "$@" > out.txt || true
  if grep -aq "$pat" out.txt; then ok "$what"; else bad "$what"; sed 's/^/       /' out.txt | head -8; snapshot; fi
}
snapshot() {   # what the machine shows and where its CPU is, for a failure
  python3 - "$ROOT/_output/bios-fail" <<'PY' 2>/dev/null || true
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect("mon.sock"); time.sleep(0.3); s.recv(65536)
s.sendall(("screendump %s.ppm\n" % sys.argv[1]).encode()); time.sleep(1); s.recv(65536)
for i in range(5):
    s.sendall(b"info registers\n"); time.sleep(0.5)
    d = s.recv(65536).decode("latin1")
    print("       cpu: " + " ".join(x for x in d.split() if x.startswith(("EIP=", "EFL=", "ESP=", "EAX=", "CR0="))) +
          " | " + " ; ".join(l[:14] for l in d.split("\n") if l.startswith(("CS ", "SS "))))
PY
  timeout 30 ssh "${OPTS[@]}" dos@127.0.0.1 'MDLOG /N' > /dev/null 2>&1 || true    # the sessions' state to the log
  grep -a "PANIC\|^kbd: \|^v86: a program\|vm: session\|^vm:   screen" -A5 debug.log | tail -30 | sed 's/^/       monitor: /'
}
boot() {   # boot name qemu-args...: wait for the SSH server
  local name=$1 i; shift
  echo "== $name"
  rm -f debug.log
  qemu-system-i386 -m 128 -display none "$@" -debugcon file:debug.log \
    -monitor unix:mon.sock,server,nowait </dev/null &
  QPID=$!
  for i in $(seq 1 120); do grep -aq "sshd: listening" debug.log 2>/dev/null && break; sleep 1; done
  if grep -aq "sshd: listening" debug.log; then ok "$name: started, SSH server listening"; return 0; fi
  bad "$name: no SSH server"; grep -a "nic:\|PANIC\|sshd" debug.log | head -5
  python3 -c 'import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.connect("mon.sock"); s.sendall(("screendump %s\n" % sys.argv[1]).encode()); time.sleep(1)' \
    "$ROOT/_output/bios-fail.ppm" 2>/dev/null || true
  return 1
}
stop() { kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true; }
NET="user,hostfwd=tcp:127.0.0.1:$PORT-:22"

if boot "IDE disk, e1000" -drive file=disk.img,format=raw,if=ide -nic $NET,model=e1000; then
  dosgrep "C: is the system disk" "MDVMM *COM" 'DIR C:\MDOS\MDVMM.COM'
  dosgrep "MDSYS: a DOS boot" "DOS boot" MDSYS
  dosgrep "PKG is there" "PKG" 'DIR C:\MDOS\PKG.EXE /B'
  dosgrep "the SSH client is there" "SSH" 'DIR C:\MDOS\SSH.EXE /B'
  # a second 32-bit program (HTTPGET) beside the SSH server: each DOS
  # session has its own DPMI client
  dosgrep "a 32-bit program beside the SSH server: a page over http" "HTTPGET: ok" 'HTTPGET http://10.0.2.2:8379/'
  dos 'COPY C:\FDCONFIG.SYS C:\KEPT.TXT' > /dev/null || true
  dos 'MDSYS REBOOT' > /dev/null 2>&1 || true
fi
stop
if boot "the same disk again" -drive file=disk.img,format=raw,if=ide -nic $NET,model=e1000; then
  dosgrep "a file written before is still there" "HIMEMX" 'TYPE C:\KEPT.TXT'
  [ "$(wc -l < "$WORK/known")" = 1 ] && ok "one host key" || bad "the host key changed"
fi
stop
if boot "USB stick (UHCI), rtl8139" -usb -drive if=none,id=stick,format=raw,file=disk.img \
     -device usb-storage,drive=stick,bootindex=0 -nic $NET,model=rtl8139; then
  dosgrep "C: is the stick" "HIMEMX" 'TYPE C:\KEPT.TXT'
  dosgrep "a page over http" "HTTPGET: ok" 'HTTPGET http://10.0.2.2:8379/'
fi
stop
# INSTALL: from the stick onto an empty hard disk, then the disk alone
truncate -s 300M target.img
if boot "stick and an empty hard disk" -usb -drive if=none,id=stick,format=raw,file=disk.img \
     -device usb-storage,drive=stick,bootindex=0 -drive file=target.img,format=raw,if=ide \
     -nic $NET,model=e1000; then
  dosgrep "INSTALL NEW: a partition on the disk" "INSTALL FORMAT" 'INSTALL NEW'
  dos 'MDSYS REBOOT' > /dev/null 2>&1 || true
  sleep 5
  for i in $(seq 1 120); do [ "$(grep -ac "sshd: listening" debug.log)" -ge 2 ] && break; sleep 1; done
  dosgrep "INSTALL FORMAT: formatted and installed" "modernDOS is installed" 'INSTALL FORMAT'
  dos 'MDSYS REBOOT' > /dev/null 2>&1 || true
  sleep 3
fi
stop
if boot "the hard disk alone" -drive file=target.img,format=raw,if=ide -nic $NET,model=e1000; then
  dosgrep "the installed system: C:\\MDOS" "MDVMM *COM" 'DIR C:\MDOS\MDVMM.COM'
  dosgrep "the volume is the formatted one" "MODERNDOS" 'VOL C:'
  [ "$(wc -l < "$WORK/known")" = 1 ] && ok "the host key came along" || bad "another host key"
fi
stop
if [ $fail = 0 ]; then echo "BIOS TEST: success"; exit 0; else echo "BIOS TEST: failure"; exit 1; fi
