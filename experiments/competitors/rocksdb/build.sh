#!/usr/bin/env bash
# Builds RocksDB v11.13.2 (extern/rocksdb) and the rocksdb_bench driver, separately from the stxxl build.
# Usage: experiments/competitors/rocksdb/build.sh [build dir]   (default: build-competitors/rocksdb)
# RocksDB is large: the first build takes a while (JOBS parallel compile jobs, default: all cores).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/rocksdb}"
JOBS="${JOBS:-$(nproc)}"

# Shallow: only the pinned commit, not RocksDB's full history.
git -C "$ROOT" submodule update --init --depth 1 extern/rocksdb

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=rocksdb
cmake --build "$BUILD" --target rocksdb_bench -j"$JOBS"

echo "RocksDB commit: $(git -C "$ROOT/extern/rocksdb" rev-parse --short HEAD)"
echo "Built: $BUILD/rocksdb_bench"
