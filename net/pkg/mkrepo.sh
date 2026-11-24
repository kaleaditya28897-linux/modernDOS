#!/bin/sh
# Make a package repository (the work of scripts/mkrepo.sh; runs where
# net/pkg/build/pkgsign exists, i.e. in the build container).
#   net/pkg/mkrepo.sh <packages> <repository> <keyfile>
set -e
S=$1; O=$2; K=$3; P=$(dirname "$0")/build/pkgsign
mkdir -p "$O"
[ -f "$K" ] || "$P" keygen "$K" > /dev/null
: > "$O/INDEX.TXT"
for d in "$S"/*/; do
  n=$(basename "$d"); f=$(echo "$n" | tr a-z A-Z | cut -c1-8).TAR
  v=$(sed -n "s/^version=//p" "$d/PKGINFO"); t=$(sed -n "s/^description=//p" "$d/PKGINFO")
  tar --format=ustar --owner=0 --group=0 -C "$d/root" -cf "$O/$f" .
  echo "$n|$v|$f|$(stat -c %s "$O/$f")|$(sha256sum "$O/$f" | cut -d" " -f1)|$t" >> "$O/INDEX.TXT"
done
"$P" sign "$K" "$O/INDEX.TXT" > "$O/INDEX.SIG"
cat "$O/INDEX.TXT"
echo "public key (key = ... in PKG.CFG): $("$P" pub "$K")"
