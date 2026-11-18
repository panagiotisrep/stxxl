#!/usr/bin/env bash
# Builds this repository's STXXL and the stxxl_map_bench / stxxl_unordered_map_bench drivers, in their own
# build directory (not the boa build). Same compiler flags and build type as a Release build of boa.
# Usage: experiments/competitors/stxxl/build.sh [build dir]   (default: build-competitors/stxxl)
# Compile-time block sizes (environment variables, also listed in README.md):
#   STXXL_MAP_NODE_BLOCK_SIZE (32768), STXXL_MAP_LEAF_BLOCK_SIZE (32768),
#   STXXL_UM_SUB_BLOCK_SIZE (8192), STXXL_UM_SUB_BLOCKS_PER_BLOCK (256)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-competitors/stxxl}"
JOBS="${JOBS:-$(nproc)}"

# STXXL's own submodules (the same ones boa needs), only if not checked out yet. Some use git@github.com:
# URLs, which need GitHub SSH keys; fetch them over https instead.
for m in extern/tlx extern/ips2ra extern/util; do
	if [[ ! -e "$ROOT/$m/.git" ]]; then
		git -C "$ROOT" -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --recursive "$m"
	fi
done

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PROJECT_stxxl_INCLUDE="$ROOT/experiments/competitors/stxxl/inject.cmake" \
      -DSTXXL_MAP_NODE_BLOCK_SIZE="${STXXL_MAP_NODE_BLOCK_SIZE:-32768}" \
      -DSTXXL_MAP_LEAF_BLOCK_SIZE="${STXXL_MAP_LEAF_BLOCK_SIZE:-32768}" \
      -DSTXXL_UM_SUB_BLOCK_SIZE="${STXXL_UM_SUB_BLOCK_SIZE:-8192}" \
      -DSTXXL_UM_SUB_BLOCKS_PER_BLOCK="${STXXL_UM_SUB_BLOCKS_PER_BLOCK:-256}"
cmake --build "$BUILD" --target stxxl_map_bench stxxl_unordered_map_bench -j"$JOBS"

echo "Built: $BUILD/stxxl_map_bench"
echo "Built: $BUILD/stxxl_unordered_map_bench"
