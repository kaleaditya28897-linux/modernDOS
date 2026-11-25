#!/bin/bash
# Build the BearSSL TLS library into /opt/bearssl (used by the
# Containerfile): TLS 1.2 for the browser and the package manager.
#   - the pinned release, checked against its SHA-256
#   - lib/libbearssl.a for DJGPP (DOS programs), lib/host/libbearssl.a for
#     the build host (tests)
#   - trust_anchors.h: the certificate authorities of the build image's
#     ca-certificates package (Mozilla's list), converted with BearSSL's
#     own tool
#
# Usage: build/bearssl.sh [tarball]   (default: download the pinned one)

set -e
VERSION=0.6
SHA256=6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14
URL=https://bearssl.org/bearssl-$VERSION.tar.gz
DEST=${BEARSSL_DEST:-/opt/bearssl}
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
cd "$DEST"
mkdir -p lib/host obj/dj obj/host

# the library, for DOS and for the host. No system random source, no
# system time on DOS: the programs give both (BR_USE_* off).
SRCS=$(find src -name '*.c' | sort)
build() {   # build compiler ar objdir out flags...
  local cc=$1 ar=$2 dir=$3 out=$4 f o; shift 4
  for f in $SRCS; do
    o=$dir/$(echo "${f#src/}" | tr / _); o=${o%.c}.o
    echo "$cc -O2 -w -Isrc -Iinc $* -c -o $o $f"
  done | xargs -P "$(nproc)" -I{} sh -c '{}'
  rm -f "$out"
  "$ar" rcs "$out" "$dir"/*.o
}
build "${DJ}gcc" "${DJ}ar" obj/dj lib/libbearssl.a -march=i586 \
  -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_USE_WIN32_RAND=0 \
  -DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_64=0 -DBR_INT128=0 -DBR_LE_UNALIGNED=1
build gcc ar obj/host lib/host/libbearssl.a

# the trust anchors, with BearSSL's tool (built for the host)
gcc -O1 -w -Isrc -Iinc -o obj/brssl tools/*.c lib/host/libbearssl.a
obj/brssl ta /etc/ssl/certs/ca-certificates.crt > trust_anchors.h 2> obj/ta.log
grep -c "^static const unsigned char TA[0-9]*_DN" trust_anchors.h | sed 's/^/trust anchors: /'
rm -rf obj test samples T0 mk conf build tools
ls -l lib/libbearssl.a lib/host/libbearssl.a trust_anchors.h
