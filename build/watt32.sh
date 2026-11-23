#!/bin/bash
# Build the Watt-32 TCP/IP library for DJGPP into /opt/watt32 (used by the
# Containerfile). Safe by construction:
#   - a pinned snapshot, checked against its SHA-256
#   - Watt-32's makefile generators (mkmake, mkdep, bin2c) are compiled
#     from their C source; the prebuilt binaries in the snapshot are not run
#   - the DJGPP errno tables come from build/watt32/ (generated under DOS by
#     build/watt32-errnos.sh)
# Result: /opt/watt32/lib/libwatt.a, headers in /opt/watt32/inc.
#
# Usage: build/watt32.sh [tarball]   (default: download the pinned one)

set -e
COMMIT=b619bfef4f9b4bcdaef20b18b3844d50f274af13      # 2026-10-14
SHA256=c298cf1ba3b54b75b89cc6b7691b3e61e7cc70bb8c0e6b7999dc240f6bf254d6
URL=https://github.com/gvanem/Watt-32/archive/$COMMIT.tar.gz
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${WATT_DEST:-/opt/watt32}
DJ=/opt/djgpp/bin/i586-pc-msdosdjgpp-

TAR=$1
if [ -z "$TAR" ]; then
  TAR=$(mktemp)
  curl -fsSL --retry 5 --retry-delay 5 -o "$TAR" "$URL"
fi
echo "$SHA256  $TAR" | sha256sum -c -
rm -rf "$DEST"
mkdir -p "$DEST"
tar -xzf "$TAR" -C "$DEST" --strip-components=1

# the generators, from source
cd "$DEST/util"
mkdir -p linux
gcc -O2 -w -o linux/mkmake mkmake.c -lslang     # S-Lang's preprocessor
gcc -O2 -w -o linux/mkdep mkdep.c
gcc -O2 -w -o linux/bin2c bin2c.c

# the errno tables for DJGPP
mkdir -p "$DEST/src/build/djgpp"
cp "$HERE/watt32/syserr.c" "$DEST/src/build/djgpp/syserr.c"
cp "$HERE/watt32/djgpp.err" "$DEST/inc/sys/djgpp.err"

# what 'configur.sh djgpp' does, with the generators built above
cd "$DEST/src"
export WATT_ROOT="$DEST"
{
  echo "BIN_PREFIX = $DJ"
  echo "W32_BIN2C_ = ../util/linux/bin2c"
  echo "W32_NASM_ ?= nasm"
  ../util/linux/mkmake -d build/djgpp makefile.all DJGPP FLAT
} > djgpp.mak
../util/linux/mkdep -s.o -p'$(OBJDIR)/' *.[ch] > build/djgpp/watt32.dep
echo "neterr.c: build/djgpp/syserr.c" >> build/djgpp/watt32.dep
make -f djgpp.mak -j4
ls -l "$DEST/lib/libwatt.a"
