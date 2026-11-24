#!/bin/sh
# Make a package repository for PKG (runs the tools in the build container).
#
#   scripts/mkrepo.sh <packages> <repository> [keyfile]
#
# <packages> has a directory per package:
#     <name>/PKGINFO      version=1.0
#                         description=What it is
#     <name>/root/...     the files, as they go below the install root
#                         (e.g. root/APPS/HELLO/HELLO.BAT -> C:\APPS\HELLO\HELLO.BAT)
# <repository> gets <NAME>.TAR for each, INDEX.TXT and INDEX.SIG; serve it
# with any web server (python3 -m http.server 8080 -d <repository>).
# keyfile (default <repository>/../repo.key, made if missing) is the
# repository's secret key; the public key for PKG.CFG is printed.
# Paths must be inside the modernDOS source tree (the container sees that).
. "$(dirname "$0")/mdenv.sh"
SRC=$1; OUT=$2; KEY=${3:-$(dirname "$2")/repo.key}
[ -d "$SRC" ] && [ -n "$OUT" ] || { sed -n '2,16p' "$0"; exit 2; }
rel() { realpath --relative-to="$ROOT" "$1"; }
mkdir -p "$OUT"
md_run sh -c 'make -s -C net/pkg host && net/pkg/mkrepo.sh "$1" "$2" "$3"' sh "$(rel "$SRC")" "$(rel "$OUT")" "$(rel "$KEY")"
