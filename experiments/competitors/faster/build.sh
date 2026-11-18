#!/usr/bin/env bash
# Builds FASTER and the faster_bench driver, separately from the stxxl build.
# Usage: experiments/competitors/faster/build.sh [build dir]   (default: build-competitors/faster)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/faster}"

git -C "$ROOT" submodule update --init extern/faster

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=faster
cmake --build "$BUILD" --target faster_bench -j"$(nproc)"

echo "FASTER version: $(git -C "$ROOT/extern/faster" describe --tags --always)"
echo "Built: $BUILD/faster_bench"
