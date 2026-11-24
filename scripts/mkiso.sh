#!/bin/sh
# Build the release ISO inside the build container (after scripts/mdbuild.sh).
#   scripts/mkiso.sh [version] [personal-dir]     (default: 0.0.1-alpha)
# personal-dir (inside the repository, e.g. _output/personal): files added
# to the boot disk, see release/build.sh. Such an image is for one's own
# machine only.
# Output: _output/release/modernDOS-<version>.iso (+ .sha256)
. "$(dirname "$0")/mdenv.sh"
if [ -n "$2" ]; then
  md_run env PERSONAL="/src/$2" release/build.sh "$1"
else
  md_run release/build.sh "$@"
fi
