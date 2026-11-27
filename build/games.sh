#!/bin/bash
# A game for the compatibility tests into /opt/games (used by the
# Containerfile): Kiloblaster, (c) 1992 Allen Pilgrim / Epic MegaGames,
# freeware since 2008 (its LICENSE.TXT comes along) - the package of the
# FreeDOS 1.4 games repository, checked against its SHA-256. Only the
# tests run it (tests/compat/game.sh); it is not part of modernDOS.
#
# Usage: build/games.sh [kiloblas.zip]   (default: download it)

set -e
SHA256=74b580c38063e21d749e76c0e087d069ce69b8ad68ae3c39d7c4ce2f6ded3da6
URL=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/games/kiloblas.zip
DEST=${GAMES_DEST:-/opt/games}

ZIP=$1
if [ -z "$ZIP" ]; then
  ZIP=$(mktemp)
  curl -fsSL --retry 5 --retry-delay 5 -o "$ZIP" "$URL"
fi
echo "$SHA256  $ZIP" | sha256sum -c -
rm -rf "$DEST/kiloblas"
mkdir -p "$DEST/kiloblas"
python3 - "$ZIP" "$DEST/kiloblas" <<'PY'
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
n = 0
for i in z.infolist():
    name = i.filename.upper()
    if name.startswith("GAMES/KILOBLAS/") and not name.endswith("/"):
        open(os.path.join(sys.argv[2], os.path.basename(name)), "wb").write(z.read(i))
        n += 1
print("kiloblas: %d files" % n)
PY
