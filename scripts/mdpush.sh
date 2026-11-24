#!/bin/sh
# Push a build to an installed modernDOS machine over SSH and try it once.
#
#   scripts/mdpush.sh <host> [iso]           upload and start the trial
#   scripts/mdpush.sh <host> keep            keep the running trial
#   scripts/mdpush.sh <host> status          what is running there
#
# The machine restarts into the new build as a TRIAL. If it comes up, test
# it and run "scripts/mdpush.sh <host> keep". If you do nothing (or it does
# not come up on the network), it restarts into the installed build after
# ten minutes by itself.
# iso: a modernDOS ISO built with scripts/mkiso.sh (default: the newest
# *personal* ISO in _output/release: the machine needs its keys).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HOST=$1
[ -n "$HOST" ] || { sed -n '2,13p' "$0"; exit 2; }
SSH="ssh -o ConnectTimeout=15 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 dos@$HOST"

case "$2" in
  keep)   $SSH KEEP; exit ;;
  status) $SSH MDSYS; exit ;;
esac
ISO=${2:-$(ls -t "$ROOT"/_output/release/*personal*.iso 2>/dev/null | head -1)}
[ -f "$ISO" ] || { echo "no ISO (build one: scripts/mkiso.sh <version> _output/personal)"; exit 1; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
echo "build: $ISO"
# the two files of a build are on the ISO's EFI partition
. "$ROOT/scripts/mdenv.sh"
cp "$ISO" "$ROOT/_output/push.iso"
md_run sh -c '
  export MTOOLS_SKIP_CHECK=1
  off=$(python3 -c "
import struct
f = open(\"_output/push.iso\", \"rb\"); f.seek(512); h = f.read(92)
lba, n, size = struct.unpack_from(\"<QII\", h, 72)
for i in range(n):
    f.seek(lba * 512 + i * size); e = f.read(size)
    if e[:16] == bytes.fromhex(\"28732ac11ff8d211ba4b00a0c93ec93b\"):
        print(struct.unpack_from(\"<Q\", e, 32)[0] * 512)
")
  mcopy -n -i _output/push.iso@@$off ::/EFI/BOOT/BOOTX64.EFI _output/push.efi
  mcopy -n -i _output/push.iso@@$off ::/MDOS/BOOT.IMG _output/push.img'
mv "$ROOT/_output/push.efi" "$W/BOOTX64.EFI"
mv "$ROOT/_output/push.img" "$W/BOOT.IMG"
rm -f "$ROOT/_output/push.iso"
printf -- '-mkdir /C/MDOS/TRY\nput %s /C/MDOS/TRY/BOOTX64.EFI\nput %s /C/MDOS/TRY/BOOT.IMG\n' \
  "$W/BOOTX64.EFI" "$W/BOOT.IMG" > "$W/batch"
sftp -b "$W/batch" "dos@$HOST"
echo "uploaded; restarting $HOST into the trial"
$SSH TRYBOOT || true
echo "When it is back:  scripts/mdpush.sh $HOST status   then   scripts/mdpush.sh $HOST keep"
