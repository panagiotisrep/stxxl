#!/usr/bin/env bash
# Runs the LeanStore experiment grid. Every setting is an environment variable (defaults below) and is
# also written into each results file by leanstore_bench itself.
#
#   BIN           leanstore_bench binary                      (default: build-competitors/leanstore/leanstore_bench)
#   OUT           results directory                         (default: results/leanstore)
#   SSD_PATH      LeanStore file (--ssd_path), on the disk under test; opened with O_DIRECT (not tmpfs)
#                 (default: $OUT/leanstore_ssd)
#   SIZES         number of keys                            (default: "10000000 100000000")
#   KEY_DISTS     key distributions                         (default: "uniform clustered seq_asc")
#   QUERY_DISTS   query distributions for --query-dist      (default: all)
#   QUERIES       queries per distribution                  (default: 20000)
#   REPEAT        repetitions per configuration             (default: 3)
#   INTERLEAVED   also run the interleaved benchmark: "ins:queries ..." pairs per batch
#                 (default: "50000:2000 50000:50000")
#   DROP_CACHES   1 = drop the OS page cache before every run (needs root)  (default: 0)
#   MEMORY_MAX    run under a cgroup memory limit via systemd-run, e.g. 1G (default: unset = no limit)
#   LEANSTORE_ARGS  extra leanstore_bench options, e.g. "--dram_gib=1"         (default: none)
#
# Example: SIZES=1000000 REPEAT=1 experiments/competitors/leanstore/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="${BIN:-$ROOT/build-competitors/leanstore/leanstore_bench}"
OUT="${OUT:-$ROOT/results/leanstore}"
SSD_PATH="${SSD_PATH:-$OUT/leanstore_ssd}"
SIZES="${SIZES:-10000000 100000000}"
KEY_DISTS="${KEY_DISTS:-uniform clustered seq_asc}"
QUERY_DISTS="${QUERY_DISTS:-all}"
QUERIES="${QUERIES:-20000}"
REPEAT="${REPEAT:-3}"
INTERLEAVED="${INTERLEAVED:-50000:2000 50000:50000}"
DROP_CACHES="${DROP_CACHES:-0}"
MEMORY_MAX="${MEMORY_MAX:-}"
LEANSTORE_ARGS="${LEANSTORE_ARGS:-}"

mkdir -p "$OUT"
[[ -x "$BIN" ]] || { echo "Missing $BIN: run experiments/competitors/leanstore/build.sh first" >&2; exit 1; }

run() {
	if [[ "$DROP_CACHES" == 1 ]]; then sync; echo 3 > /proc/sys/vm/drop_caches; fi
	local cmd=("$BIN" --ssd_path="$SSD_PATH" $LEANSTORE_ARGS "$@")
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
	for pair in $INTERLEAVED; do
		for ((r = 1; r <= REPEAT; ++r)); do
			run --interleaved --size "$size" --insertions-per-batch "${pair%%:*}" --queries-per-batch "${pair##*:}"
		done
	done
done
