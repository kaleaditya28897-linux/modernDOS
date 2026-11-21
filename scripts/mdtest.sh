#!/bin/sh
# Boot-test the built kernel in QEMU (headless) inside the build container.
. "$(dirname "$0")/mdenv.sh"
KERNEL=${1:-_output/gcc/kgc8632.sys}
md_run bash -c "cd test && ./test.sh ../$KERNEL diskgc bootgc 'boot gcc: '"
