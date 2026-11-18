#!/usr/bin/env bash
# Runs the SILT experiment grid. Every setting is an environment variable (defaults below) and is
# also written into each results file by silt_bench itself.
#
#   BIN           silt_bench binary                      (default: build-competitors/silt/silt_bench)
#   OUT           results directory                         (default: results/silt)
#   DB_DIR        SILT files directory, on the disk under test (default: $OUT/db)
#   SIZES         number of keys                            (default: "10000000 100000000")
#   KEY_DISTS     key distributions                         (default: "uniform clustered seq_asc")
#   QUERY_DISTS   query distributions for --query-dist      (default: all)
#   QUERIES       queries per distribution                  (default: 20000)
#   REPEAT        repetitions per configuration             (default: 3)
#   INTERLEAVED   also run the interleaved benchmark: "ins:queries ..." pairs per batch
#                 (default: "50000:2000 50000:50000")
#   DROP_CACHES   1 = drop the OS page cache before every run (needs root)  (default: 0)
#   MEMORY_MAX    run under a cgroup memory limit via systemd-run, e.g. 1G (default: unset = no limit)
#   SILT_ARGS  extra silt_bench options, e.g. "--mode sorted" (no INTERLEAVED then) (default: none)
#
# Example: SIZES=1000000 REPEAT=1 experiments/competitors/silt/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="${BIN:-$ROOT/build-competitors/silt/silt_bench}"
OUT="${OUT:-$ROOT/results/silt}"
DB_DIR="${DB_DIR:-$OUT/db}"
SIZES="${SIZES:-10000000 100000000}"
KEY_DISTS="${KEY_DISTS:-uniform clustered seq_asc}"
QUERY_DISTS="${QUERY_DISTS:-all}"
QUERIES="${QUERIES:-20000}"
REPEAT="${REPEAT:-3}"
INTERLEAVED="${INTERLEAVED:-50000:2000 50000:50000}"
DROP_CACHES="${DROP_CACHES:-0}"
MEMORY_MAX="${MEMORY_MAX:-}"
SILT_ARGS="${SILT_ARGS:-}"

mkdir -p "$OUT"
[[ -x "$BIN" ]] || { echo "Missing $BIN: run experiments/competitors/silt/build.sh first" >&2; exit 1; }

run() {
	if [[ "$DROP_CACHES" == 1 ]]; then sync; echo 3 > /proc/sys/vm/drop_caches; fi
	local cmd=("$BIN" --db-dir "$DB_DIR" $SILT_ARGS "$@")
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
	[[ " $SILT_ARGS " == *" --mode sorted "* ]] && continue # sorted store is static: no interleaved runs
	for pair in $INTERLEAVED; do
		for ((r = 1; r <= REPEAT; ++r)); do
			run --interleaved --size "$size" --insertions-per-batch "${pair%%:*}" --queries-per-batch "${pair##*:}"
		done
	done
done
