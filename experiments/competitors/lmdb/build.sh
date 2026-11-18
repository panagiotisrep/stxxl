#!/usr/bin/env bash
# Builds LMDB and the lmdb_bench driver, separately from the stxxl build.
# Usage: experiments/competitors/lmdb/build.sh [build dir]   (default: build-competitors/lmdb)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/lmdb}"

git -C "$ROOT" submodule update --init extern/lmdb

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=lmdb
cmake --build "$BUILD" --target lmdb_bench -j"$(nproc)"

echo "LMDB version: $(git -C "$ROOT/extern/lmdb" describe --tags --always)"
echo "Built: $BUILD/lmdb_bench"
