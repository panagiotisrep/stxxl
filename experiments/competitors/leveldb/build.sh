#!/usr/bin/env bash
# Builds LevelDB 1.23 (extern/leveldb) and the leveldb_bench driver, separately from the stxxl build.
# Usage: experiments/competitors/leveldb/build.sh [build dir]   (default: build-competitors/leveldb)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/leveldb}"

# LevelDB's own submodules (googletest, benchmark) are only needed for its tests: not --recursive.
git -C "$ROOT" submodule update --init extern/leveldb

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=leveldb
cmake --build "$BUILD" --target leveldb_bench -j"$(nproc)"

echo "LevelDB commit: $(git -C "$ROOT/extern/leveldb" describe --tags --always)"
echo "Built: $BUILD/leveldb_bench"
