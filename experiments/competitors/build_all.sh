#!/usr/bin/env bash
# Downloads (git submodules, pinned commits) and builds every competitor, each with its own
# <competitor>/build.sh, into build-competitors/<competitor>/ (stxxl: this repository's STXXL map and unordered_map).
#
# Usage: experiments/competitors/build_all.sh
#   COMPETITORS  which ones (default: "leveldb rocksdb pachash leanstore stxxl silt")
#   JOBS         parallel compile jobs per build (default: all cores; RocksDB needs ~1 GB RAM per job)
#   BLOCK        I/O block size in bytes for the competitors whose block size is fixed at compile time
#                (default: 32768 = BOA_BLOCK_SIZE; must equal run_all.sh's BLOCK):
#                PaCHash block length, STXXL map node/leaf blocks, STXXL unordered_map sub-blocks
#                (with 1 sub-block per block, so that its block, the unit it caches, is BLOCK too)
#   PACHASH_BLOCK_LENGTH, STXXL_MAP_NODE_BLOCK_SIZE, STXXL_MAP_LEAF_BLOCK_SIZE, STXXL_UM_SUB_BLOCK_SIZE,
#   STXXL_UM_SUB_BLOCKS_PER_BLOCK   override single values (default: from BLOCK, see above)
#   PACHASH_WITH_URING   passed on to pachash/build.sh (see pachash/README.md)
#
# Needs git, CMake ≥ 3.25, a C++20 compiler and the system packages listed in README.md
# ("System packages"); on Ubuntu/Debian: experiments/competitors/install_dependencies.sh
# A failing build does not stop the others; the summary at the end lists each result.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
COMPETITORS="${COMPETITORS:-leveldb rocksdb pachash leanstore stxxl silt}"
BLOCK="${BLOCK:-32768}"
export PACHASH_BLOCK_LENGTH="${PACHASH_BLOCK_LENGTH:-$BLOCK}"
export STXXL_MAP_NODE_BLOCK_SIZE="${STXXL_MAP_NODE_BLOCK_SIZE:-$BLOCK}"
export STXXL_MAP_LEAF_BLOCK_SIZE="${STXXL_MAP_LEAF_BLOCK_SIZE:-$BLOCK}"
export STXXL_UM_SUB_BLOCK_SIZE="${STXXL_UM_SUB_BLOCK_SIZE:-$BLOCK}"
um_per_block=1 # one sub-block per block: the unordered_map block is BLOCK (it caches whole blocks)
export STXXL_UM_SUB_BLOCKS_PER_BLOCK="${STXXL_UM_SUB_BLOCKS_PER_BLOCK:-$um_per_block}"
echo "Block sizes: PaCHash $PACHASH_BLOCK_LENGTH, STXXL map node/leaf $STXXL_MAP_NODE_BLOCK_SIZE/$STXXL_MAP_LEAF_BLOCK_SIZE, STXXL unordered_map sub-block $STXXL_UM_SUB_BLOCK_SIZE x $STXXL_UM_SUB_BLOCKS_PER_BLOCK"
LOGS="$ROOT/build-competitors/logs"
mkdir -p "$LOGS"

declare -A result
for c in $COMPETITORS; do
	if [[ ! -x "$DIR/$c/build.sh" ]]; then
		result[$c]="unknown competitor"
		continue
	fi
	echo "=== $c (log: $LOGS/$c.log)"
	start=$SECONDS
	if "$DIR/$c/build.sh" "$ROOT/build-competitors/$c" > "$LOGS/$c.log" 2>&1; then
		result[$c]="ok    $ROOT/build-competitors/$c/ ($((SECONDS - start)) s)"
	else
		result[$c]="FAILED, see $LOGS/$c.log"
		tail -5 "$LOGS/$c.log"
	fi
done

echo
echo "Summary:"
status=0
for c in $COMPETITORS; do
	printf '  %-10s %s\n' "$c" "${result[$c]}"
	[[ "${result[$c]}" == ok* ]] || status=1
done
exit $status
