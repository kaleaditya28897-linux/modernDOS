#!/bin/bash
# Put the iPXE source tree into /opt/ipxe (used by the Containerfile): the
# monitor's network card drivers are built from it (mdvmm/ipxe). A pinned
# snapshot, checked against its SHA-256; nothing of it is run here.
#
# Usage: build/ipxe.sh [tarball]   (default: download the pinned one)

set -e
COMMIT=4b7e1df9c513d43e9e79c63463aa2470f34de7e3      # 2026-10-24
SHA256=a767f18394581a46e0cd2811166698bf65ee85c8de1b6b44be8558a6adc078ba
URL=https://github.com/ipxe/ipxe/archive/$COMMIT.tar.gz
DEST=${IPXE_DEST:-/opt/ipxe}

TAR=$1
if [ -z "$TAR" ]; then
  TAR=$(mktemp)
  curl -fsSL --retry 5 --retry-delay 5 -o "$TAR" "$URL"
fi
echo "$SHA256  $TAR" | sha256sum -c -
rm -rf "$DEST"
mkdir -p "$DEST"
# only the source and the licences (no tests, tools, documentation)
tar -xzf "$TAR" -C "$DEST" --strip-components=1 --wildcards \
  '*/src/include' '*/src/arch/x86' '*/src/arch/i386' '*/src/config' '*/src/core' \
  '*/src/net' '*/src/drivers' '*/COPYING*'
du -sh "$DEST"
