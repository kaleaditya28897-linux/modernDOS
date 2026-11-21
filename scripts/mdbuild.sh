#!/bin/sh
# Build the modernDOS kernel inside the build container.
#   scripts/mdbuild.sh [gcc|wc|all]     (default: all)
# Output: _output/gcc/kgc8632.sys, _output/wc/kwc8632.sys (+ .map, country.sys),
#         mdvmm/build/MDVMM.COM
. "$(dirname "$0")/mdenv.sh"
WHICH=${1:-all}
md_run sh -c '
  set -e
  build() {   # build <COMPILER> <outdir> <prefix>
    make -C country clean >/dev/null
    make clean >/dev/null 2>&1 || true
    # artifacts of the other compiler are not removed by make clean
    find lib drivers kernel boot sys utils setver \( -name "*.obj" -o \
      -name "*.lib" -o -name "*.lnk" -o -name "*.o" \) -delete
    make all COMPILER=$1
    mkdir -p _output/$2
    mv -f bin/$3*.map bin/$3*.sys bin/country.sys _output/$2/
  }
  case '"$WHICH"' in gcc|all) build gcc gcc kgc ;; esac
  case '"$WHICH"' in wc|all)
    export PATH=/src/bin:$PATH:$WATCOM/binl64
    build owlinux wc kwc ;;
  esac
  make -s -C mdvmm
  ls -l _output/*/k*.sys mdvmm/build/MDVMM.COM
'
