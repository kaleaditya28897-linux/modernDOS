#!/bin/bash
# Build the iPXE network drivers for the monitor: one relocatable object
# whose only global names are the ipxe_* functions of glue.c.
#
# Usage: mdvmm/ipxe/build.sh <iPXE source tree> <output.o> [debug-objects]
#   debug-objects: e.g. "sky2" or "sky2,netdevice" - iPXE's debug messages
#   of those source files go to the monitor's log

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(realpath "$1")/src
OUT=$(realpath -m "$2")
DEBUG=$3
[ -f "$SRC/include/compiler.h" ] || { echo "no iPXE source tree at $1"; exit 1; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CC=i686-linux-gnu-gcc

# the empty local configuration headers iPXE's makefile would create
mkdir -p "$W/overlay/config/local"
grep -rhoE "config/local/[a-z_0-9]+\.h" "$SRC/config" "$SRC/include" | sort -u |
  while read -r f; do : > "$W/overlay/$f"; done

CFLAGS="-m32 -march=i686 -Os -ffreestanding -fno-builtin -fno-pic -fno-pie
  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
  -fno-strict-aliasing -fshort-wchar -fcommon -mgeneral-regs-only
  -mpreferred-stack-boundary=2 -w
  -DARCH=i386 -DPLATFORM=mdvmm -DPLATFORM_mdvmm -DSECUREBOOT=0 -DNVALGRIND
  -DASM_TCHAR=@ -DASM_TCHAR_OPS=@ -DPROFILING=0 -DASSERTING=0 -DNDEBUG
  -I$HERE/overlay -I$W/overlay -I$SRC/arch/i386/include -I$SRC/arch/x86/include
  -I$SRC/include -I$SRC -include $SRC/include/compiler.h"

objs=
compile() {   # compile dir file
  local name o dbg=
  name=$(basename "$2" .c | tr - _)
  o="$W/$name.o"
  case ",$DEBUG," in *",$name,"*) dbg="-Ddebug_$name=1" ;; esac
  (cd "$1" && $CC $CFLAGS $dbg -DOBJECT="$name" -c "$2" -o "$o")
  objs="$objs $o"
}
for f in $(grep -hv '^#' "$HERE/core.list" "$HERE/drivers.list"); do
  compile "$SRC" "$f"
done
compile "$HERE" glue.c
# without iPXE's headers
$CC -m32 -Os -ffreestanding -fno-pic -fno-pie -c "$HERE/glue_data.c" -o "$W/glue_data.o"

i686-linux-gnu-ld -m elf_i386 -r -T "$HERE/ipxe.lds" -o "$W/ipxe.o" $objs "$W/glue_data.o"
i686-linux-gnu-objcopy -G ipxe_init -G ipxe_get -G ipxe_send -G ipxe_recv -G ipxe_link \
  -G ipxe_debug -G ipxe_set_virt_offset "$W/ipxe.o" "$OUT"
und=$(i686-linux-gnu-nm -u "$OUT" | awk '{print $2}' | grep -v '^mdvmm_' || true)
if [ -n "$und" ]; then echo "iPXE drivers: unresolved:" $und; exit 1; fi
i686-linux-gnu-size "$OUT"
