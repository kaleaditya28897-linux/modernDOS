#!/bin/sh
# Build the release ISO inside the build container (after scripts/mdbuild.sh).
#   scripts/mkiso.sh [version]     (default: 0.0.1-alpha)
# Output: _output/release/modernDOS-<version>.iso (+ .sha256)
. "$(dirname "$0")/mdenv.sh"
md_run release/build.sh "$@"
