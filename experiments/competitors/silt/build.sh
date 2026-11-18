#!/usr/bin/env bash
# Builds SILT (extern/silt: ByteHamster/silt build-fix branch, commit 55ee50b) and the silt_bench driver,
# separately from the stxxl build.
# Usage: experiments/competitors/silt/build.sh [build dir]   (default: build-competitors/silt)
#
# System packages SILT needs (Ubuntu/Debian):
#   sudo apt-get install libxerces-c-dev libssl-dev libgsl-dev libtbb-dev libboost-dev
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/silt}"
JOBS="${JOBS:-$(nproc)}"

git -C "$ROOT" submodule update --init extern/silt

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=silt
cmake --build "$BUILD" --target silt_bench -j"$JOBS"

echo "SILT commit: $(git -C "$ROOT/extern/silt" rev-parse --short HEAD)"
echo "Built: $BUILD/silt_bench"
