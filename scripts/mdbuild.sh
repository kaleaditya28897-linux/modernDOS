#!/bin/sh
# Build the modernDOS kernel (GCC/ia16, 386, FAT32) inside the build container.
# Output: _output/gcc/kgc8632.sys (+ .map, country.sys)
. "$(dirname "$0")/mdenv.sh"
md_run sh -c '
  set -e
  make -C country clean >/dev/null
  make clean >/dev/null 2>&1 || true
  make all COMPILER=gcc
  mkdir -p _output/gcc
  mv -f bin/kgc*.map bin/kgc*.sys bin/country.sys _output/gcc/
  ls -l _output/gcc
'
