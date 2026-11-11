#!/usr/bin/env bash
# Runs the PaCHash experiment grid. Every setting is an environment variable (defaults below) and is
# also written into each results file by pachash_bench itself.
# PaCHash is static: there is no interleaved benchmark. Compare with boa --suspended / --materialization.
#
#   BIN           pachash_bench binary                      (default: build-competitors/pachash/pachash_bench)
#   OUT           results directory                         (default: results/pachash)
#   STORE_FILE    object store file, on the disk under test (default: $OUT/pachash_store.db)
#                 O_DIRECT (the default) needs a file system that supports it (not tmpfs)
#   SIZES         number of keys                            (default: "10000000 100000000")
#   KEY_DISTS     key distributions                         (default: "uniform clustered seq_asc")
#   QUERY_DISTS   query distributions for --query-dist      (default: all)
#   QUERIES       queries per distribution                  (default: 20000)
#   REPEAT        repetitions per configuration             (default: 3)
#   DROP_CACHES   1 = drop the OS page cache before every run (needs root)  (default: 0)
#   MEMORY_MAX    run under a cgroup memory limit via systemd-run, e.g. 1G (default: unset = no limit)
#   PACHASH_ARGS  extra pachash_bench options, e.g. "--a 16 --io io_submit --queue-depth 32" (default: none)
#
# Example: SIZES=1000000 REPEAT=1 experiments/competitors/pachash/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="${BIN:-$ROOT/build-competitors/pachash/pachash_bench}"
OUT="${OUT:-$ROOT/results/pachash}"
STORE_FILE="${STORE_FILE:-$OUT/pachash_store.db}"
SIZES="${SIZES:-10000000 100000000}"
KEY_DISTS="${KEY_DISTS:-uniform clustered seq_asc}"
QUERY_DISTS="${QUERY_DISTS:-all}"
QUERIES="${QUERIES:-20000}"
REPEAT="${REPEAT:-3}"
DROP_CACHES="${DROP_CACHES:-0}"
MEMORY_MAX="${MEMORY_MAX:-}"
PACHASH_ARGS="${PACHASH_ARGS:-}"

mkdir -p "$OUT"
[[ -x "$BIN" ]] || { echo "Missing $BIN: run experiments/competitors/pachash/build.sh first" >&2; exit 1; }

run() {
	if [[ "$DROP_CACHES" == 1 ]]; then sync; echo 3 > /proc/sys/vm/drop_caches; fi
	local cmd=("$BIN" --store-file "$STORE_FILE" $PACHASH_ARGS "$@")
	if [[ -n "$MEMORY_MAX" ]]; then
		cmd=(systemd-run --user --scope --quiet -p MemoryMax="$MEMORY_MAX" -p MemorySwapMax=0 "${cmd[@]}")
	fi
	echo "+ ${cmd[*]}"
	(cd "$OUT" && "${cmd[@]}")
}

for size in $SIZES; do
	for kd in $KEY_DISTS; do
		for ((r = 1; r <= REPEAT; ++r)); do
			run --size "$size" --queries "$QUERIES" --key-dist "$kd" --query-dist "$QUERY_DISTS"
		done
	done
done
