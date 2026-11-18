#!/usr/bin/env bash
# Builds SplinterDB (extern/splinterdb, commit 73f89d8) as a static library with its own Makefile and the
# splinterdb_bench driver, separately from the stxxl build.
# Needs libaio-dev and libxxhash-dev (experiments/competitors/install_dependencies.sh).
# Usage: experiments/competitors/splinterdb/build.sh [build dir]   (default: build-competitors/splinterdb)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/splinterdb}"

git -C "$ROOT" submodule update --init extern/splinterdb

# Release mode, out of tree: $BUILD/splinterdb-lib/release/lib/libsplinterdb.a
make -C "$ROOT/extern/splinterdb" -j"$(nproc)" BUILD_ROOT="$BUILD/splinterdb-lib" BUILD_MODE=release CC=gcc LD=gcc libs

cmake -S "$ROOT/experiments/competitors" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCOMPETITOR=splinterdb \
	-DSPLINTERDB_LIB_DIR="$BUILD/splinterdb-lib/release"
cmake --build "$BUILD" --target splinterdb_bench -j"$(nproc)"

echo "SplinterDB commit: $(git -C "$ROOT/extern/splinterdb" describe --tags --always)"
echo "Built: $BUILD/splinterdb_bench"
