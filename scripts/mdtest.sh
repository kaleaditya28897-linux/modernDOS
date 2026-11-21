#!/bin/sh
# Run the modernDOS test suite in QEMU (headless) inside the build container:
#  - upstream boot test (test/test.sh)
#  - long filename API test (tests/lfn/run.sh)
. "$(dirname "$0")/mdenv.sh"
KERNEL=${1:-_output/gcc/kgc8632.sys}
md_run bash -c "
  rc=0
  (cd test && ./test.sh ../$KERNEL diskgc bootgc 'boot gcc: ') || rc=1
  tests/lfn/run.sh $KERNEL || rc=1
  exit \$rc
"
