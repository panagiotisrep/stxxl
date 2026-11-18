#!/usr/bin/env bash
# Builds PaCHash (extern/pachash, commit f7f18fd) and the pachash_bench driver, separately from the stxxl build.
# Usage: experiments/competitors/pachash/build.sh [build dir]   (default: build-competitors/pachash)
# Build settings (environment variables, also listed in README.md):
#   PACHASH_BLOCK_LENGTH  PaCHash block size in bytes (default 4096, PaCHash's default)
#   PACHASH_WITH_URING    ON = build with io_uring, requires liburing (default OFF)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/pachash}"
PACHASH_BLOCK_LENGTH="${PACHASH_BLOCK_LENGTH:-4096}"
PACHASH_WITH_URING="${PACHASH_WITH_URING:-OFF}"

# PaCHash's own .gitmodules use git@github.com: URLs, which need GitHub SSH keys; fetch them over https.
# Only the submodules the library needs (not extern/lz4, which is for one of PaCHash's examples).
HTTPS=(-c url."https://github.com/".insteadOf="git@github.com:")
git -C "$ROOT" submodule update --init extern/pachash
git -C "$ROOT/extern/pachash" "${HTTPS[@]}" submodule update --init extern/tlx extern/ips2ra extern/util
git -C "$ROOT/extern/pachash/extern/ips2ra" "${HTTPS[@]}" submodule update --init --recursive
git -C "$ROOT/extern/pachash/extern/util" "${HTTPS[@]}" submodule update --init --recursive

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=pachash \
      -DPACHASH_BLOCK_LENGTH="$PACHASH_BLOCK_LENGTH" -DPACHASH_WITH_URING="$PACHASH_WITH_URING"
cmake --build "$BUILD" --target pachash_bench -j"$(nproc)"

echo "PaCHash commit: $(git -C "$ROOT/extern/pachash" rev-parse --short HEAD)"
echo "Built: $BUILD/pachash_bench (block length $PACHASH_BLOCK_LENGTH, io_uring $PACHASH_WITH_URING)"
