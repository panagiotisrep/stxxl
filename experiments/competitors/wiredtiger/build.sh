#!/usr/bin/env bash
# Builds WiredTiger (extern/wiredtiger, tag mongodb-9.0.8) as a static library and the wiredtiger_bench driver,
# separately from the stxxl build.
# Usage: experiments/competitors/wiredtiger/build.sh [build dir]   (default: build-competitors/wiredtiger)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/wiredtiger}"

git -C "$ROOT" submodule update --init extern/wiredtiger

# WiredTiger on its own: static, no compressors / Python / strict warnings / tests (its defaults for everything else).
cmake -S "$ROOT/extern/wiredtiger" -B "$BUILD/wiredtiger-lib" -DCMAKE_BUILD_TYPE=Release \
	-DENABLE_STATIC=ON -DENABLE_SHARED=OFF -DENABLE_PYTHON=OFF -DENABLE_STRICT=OFF \
	-DENABLE_SNAPPY=OFF -DENABLE_LZ4=OFF -DENABLE_ZLIB=OFF -DENABLE_ZSTD=OFF -DENABLE_SODIUM=OFF
cmake --build "$BUILD/wiredtiger-lib" --target wiredtiger_static -j"$(nproc)"

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=wiredtiger \
	-DWIREDTIGER_LIB_DIR="$BUILD/wiredtiger-lib"
cmake --build "$BUILD" --target wiredtiger_bench -j"$(nproc)"

echo "WiredTiger version: $(git -C "$ROOT/extern/wiredtiger" describe --tags --always)"
echo "Built: $BUILD/wiredtiger_bench"
