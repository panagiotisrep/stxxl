#!/usr/bin/env bash
# Builds LeanStore (extern/leanstore, commit fcb64bf) and the leanstore_bench driver, separately from the
# stxxl build. Usage: experiments/competitors/leanstore/build.sh [build dir]  (default: build-competitors/leanstore)
#
# System packages LeanStore needs (Ubuntu/Debian): sudo apt-get install cmake libtbb-dev libaio-dev
# LeanStore downloads gflags, tabulate and rapidjson itself at build time (pinned commits, see its libs/).
# LeanStore uses x86 flags (-mavx2 -mcx16): x86-64 CPU with AVX2 needed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/leanstore}"
JOBS="${JOBS:-$(nproc)}"

# Shallow: only the pinned commit.
git -C "$ROOT" submodule update --init --depth 1 extern/leanstore

# LeanStore is configured as its own top-level project; inject.cmake adds the driver target to it.
cmake -S "$ROOT/extern/leanstore" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PROJECT_leanstoredb_INCLUDE="$ROOT/experiments/competitors/leanstore/inject.cmake"
cmake --build "$BUILD" --target leanstore_bench -j"$JOBS"

echo "LeanStore commit: $(git -C "$ROOT/extern/leanstore" rev-parse --short HEAD)"
echo "Built: $BUILD/leanstore_bench"
