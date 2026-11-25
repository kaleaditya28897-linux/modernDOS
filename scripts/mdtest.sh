#!/bin/sh
# Run the modernDOS test suite in QEMU (headless) inside the build container:
#  - upstream boot test (test/test.sh) for every kernel that was built
#  - long filename API + DJGPP libc test (tests/lfn/run.sh), GCC kernel
#  - MDVMM supervisor: DOS + tests inside the V86 monitor (tests/mdvmm/run.sh)
#  - UEFI boot: OVMF -> BOOTX64.EFI -> MDVMM core + virtual BIOS -> DOS
#    (tests/uefi/run.sh)
#  - release ISO, if built (scripts/mkiso.sh): UEFI CD/USB, BIOS CD boots
#    (tests/iso/run.sh)
. "$(dirname "$0")/mdenv.sh"
md_run bash -c '
  rc=0
  for k in _output/gcc/kgc8632.sys _output/wc/kwc8632.sys; do
    [ -f "$k" ] || continue
    (cd test && ./test.sh ../$k disk_t boot_t "boot $(basename $k): ") || rc=1
  done
  tests/lfn/run.sh _output/gcc/kgc8632.sys || rc=1
  tests/mdvmm/run.sh _output/gcc/kgc8632.sys || rc=1
  tests/uefi/run.sh _output/gcc/kgc8632.sys || rc=1
  tests/usb/run.sh _output/gcc/kgc8632.sys || rc=1
  tests/net/run.sh _output/gcc/kgc8632.sys || rc=1
  tests/ssh/host.sh || rc=1
  tests/ssh/run.sh _output/gcc/kgc8632.sys || rc=1
  tests/install/run.sh _output/gcc/kgc8632.sys || rc=1
  tests/bios/run.sh || rc=1
  tests/pkg/host.sh || rc=1
  tests/pkg/run.sh _output/gcc/kgc8632.sys || rc=1
  for iso in _output/release/*.iso; do
    [ -f "$iso" ] && { tests/iso/run.sh "$iso" || rc=1; }
  done
  exit $rc
'
