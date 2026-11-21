#!/bin/sh
# Shared helpers: build the container image on demand and run a command in it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${MD_IMAGE:-localhost/moderndos-build:latest}
ENGINE=${MD_ENGINE:-podman}

md_image() {
  if ! $ENGINE image exists "$IMAGE" 2>/dev/null; then
    $ENGINE build -t "$IMAGE" -f "$ROOT/build/Containerfile" "$ROOT/build"
  fi
}

md_run() {
  md_image
  $ENGINE run --rm --userns=keep-id -v "$ROOT":/src:Z -w /src "$IMAGE" "$@"
}
