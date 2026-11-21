#!/bin/sh
# Run the modernDOS test suite in QEMU (headless) inside the build container:
#  - upstream boot test (test/test.sh) for every kernel that was built
#  - long filename API + DJGPP libc test (tests/lfn/run.sh), GCC kernel
. "$(dirname "$0")/mdenv.sh"
md_run bash -c '
  rc=0
  for k in _output/gcc/kgc8632.sys _output/wc/kwc8632.sys; do
    [ -f "$k" ] || continue
    (cd test && ./test.sh ../$k disk_t boot_t "boot $(basename $k): ") || rc=1
  done
  tests/lfn/run.sh _output/gcc/kgc8632.sys || rc=1
  exit $rc
'
