#!/bin/bash
# Package manager test on Linux (runs inside the build container): the
# PKG code against a repository made with net/pkg/mkrepo.sh and served by
# a local web server: update, list, install, remove; a tampered package
# list, a tampered package and a wrong key must be refused.
#
# Usage: tests/pkg/host.sh

set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'kill $WEB 2>/dev/null || true; rm -rf "$WORK"' EXIT
make -s -C "$ROOT/net/pkg" host
cd "$WORK"
mkdir -p src/hello/root/APPS/HELLO src/tools/root/BIN inst
printf 'version=1.2\ndescription=Says hello\n' > src/hello/PKGINFO
printf '@ECHO OFF\r\nECHO Hello from a package\r\n' > src/hello/root/APPS/HELLO/HELLO.BAT
head -c 70000 /dev/urandom > src/hello/root/APPS/HELLO/DATA.BIN
printf 'version=0.9\ndescription=Two small tools\n' > src/tools/PKGINFO
echo one > src/tools/root/BIN/ONE.TXT
echo two > src/tools/root/BIN/TWO.TXT
"$ROOT/net/pkg/mkrepo.sh" src repo repo.key > mkrepo.log
KEY=$(sed -n 's/^public key.*: //p' mkrepo.log)
python3 -m http.server 8377 --bind 127.0.0.1 -d repo > /dev/null 2>&1 &
WEB=$!
sleep 1
cp "$ROOT/net/pkg/build/pkg-host" inst/pkg
printf 'repo = http://127.0.0.1:8377\nkey = %s\nroot = %s/inst/\n' "$KEY" "$WORK" > inst/PKG.CFG
PKG="$WORK/inst/pkg"

fail=0
check() { local what=$1; shift; if "$@" > out.txt 2>&1; then echo "  ok   $what"; else
  echo "  FAIL $what"; sed 's/^/       /' out.txt | head -10; fail=1; fi; }

check "update: signed package list accepted" bash -c "$PKG UPDATE | grep -q '2 packages (signature good)'"
check "list: both packages, none installed" bash -c "$PKG LIST | grep -c '^\[ \]' | grep -qx 2"
check "install hello" bash -c "$PKG INSTALL hello | grep -q 'Installed hello 1.2: 2 files'"
check "the files are there, unchanged" bash -c "cmp src/hello/root/APPS/HELLO/DATA.BIN inst/APPS/HELLO/DATA.BIN && cmp src/hello/root/APPS/HELLO/HELLO.BAT inst/APPS/HELLO/HELLO.BAT"
check "list: hello is installed" bash -c "$PKG LIST | grep -q '^\[\*\] hello'"
check "remove hello" bash -c "$PKG REMOVE hello | grep -q 'Removed hello: 2 files' && test ! -e inst/APPS/HELLO/DATA.BIN"
# tampering
cp repo/TOOLS.TAR tools.good
printf 'evil' | dd of=repo/TOOLS.TAR bs=1 seek=600 conv=notrunc status=none
check "a changed package is refused" bash -c "! $PKG INSTALL tools > o 2>&1 && grep -q 'does NOT match' o && test ! -e inst/BIN/ONE.TXT"
cp tools.good repo/TOOLS.TAR
check "the good package installs" bash -c "$PKG INSTALL tools | grep -q '2 files'"
sed -i 's/Says hello/Says hello!/' repo/INDEX.TXT
check "a changed package list is refused" bash -c "! $PKG UPDATE > o 2>&1 && grep -q 'signature does NOT fit' o"
"$ROOT/net/pkg/mkrepo.sh" src repo other.key > /dev/null
check "a list signed with another key is refused" bash -c "! $PKG UPDATE > o 2>&1 && grep -q 'signature does NOT fit' o"
mkdir -p src/evil/root; printf 'version=1\ndescription=escapes\n' > src/evil/PKGINFO; echo x > src/evil/root/ok.txt
"$ROOT/net/pkg/mkrepo.sh" src repo repo.key > /dev/null
tar --format=ustar -P -cf repo/EVIL.TAR --transform 's|^|../|' -C src/evil/root ok.txt 2>/dev/null
sed -i "s#^evil|.*#evil|1|EVIL.TAR|$(stat -c %s repo/EVIL.TAR)|$(sha256sum repo/EVIL.TAR | cut -d' ' -f1)|escapes#" repo/INDEX.TXT
"$ROOT/net/pkg/build/pkgsign" sign repo.key repo/INDEX.TXT > repo/INDEX.SIG
$PKG UPDATE > /dev/null
check "a package cannot write outside the install root" bash -c "$PKG INSTALL evil > o 2>&1; test ! -e $WORK/ok.txt && grep -q '0 files' o"

if [ $fail = 0 ]; then echo "PKG HOST TEST: success"; exit 0; else echo "PKG HOST TEST: failure"; exit 1; fi
