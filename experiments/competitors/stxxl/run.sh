#!/usr/bin/env bash
# Runs the STXXL map / unordered_map experiment grid. Every setting is an environment variable (defaults
# below) and is also written into each results file by the drivers themselves.
#
#   BUILD         build directory with both binaries         (default: build-competitors/stxxl)
#   STRUCTURES    which structures                          (default: "map unordered_map")
#   OUT           results directory                         (default: results/stxxl)
#   SIZES         number of keys                            (default: "10000000 100000000")
#   KEY_DISTS     key distributions                         (default: "uniform clustered seq_asc")
#   QUERY_DISTS   query distributions for --query-dist      (default: all)
#   QUERIES       queries per distribution                  (default: 20000)
#   REPEAT        repetitions per configuration             (default: 3)
#   INTERLEAVED   also run the interleaved benchmark: "ins:queries ..." pairs per batch
#                 (default: "50000:2000 50000:50000")
#   DROP_CACHES   1 = drop the OS page cache before every run (needs root)  (default: 0)
#   MEMORY_MAX    run under a cgroup memory limit via systemd-run, e.g. 1G (default: unset = no limit)
#   MAP_ARGS      extra stxxl_map_bench options, e.g. "--node-cache 1048576 --leaf-cache 1048576"
#   UM_ARGS       extra stxxl_unordered_map_bench options, e.g. "--buffer-size 1048576"
#   STXXL_CONFIG  STXXL disk configuration file (use the same one as for boa); STXXL otherwise looks for
#                 .stxxl in the current directory ($OUT), then uses its default disk
#
# Example: SIZES=1000000 REPEAT=1 experiments/competitors/stxxl/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-competitors/stxxl}"
STRUCTURES="${STRUCTURES:-map unordered_map}"
OUT="${OUT:-$ROOT/results/stxxl}"
SIZES="${SIZES:-10000000 100000000}"
KEY_DISTS="${KEY_DISTS:-uniform clustered seq_asc}"
QUERY_DISTS="${QUERY_DISTS:-all}"
QUERIES="${QUERIES:-20000}"
REPEAT="${REPEAT:-3}"
INTERLEAVED="${INTERLEAVED:-50000:2000 50000:50000}"
DROP_CACHES="${DROP_CACHES:-0}"
MEMORY_MAX="${MEMORY_MAX:-}"
MAP_ARGS="${MAP_ARGS:-}"
UM_ARGS="${UM_ARGS:-}"

mkdir -p "$OUT"

run() {
	local structure="$1"; shift
	local bin="$BUILD/stxxl_${structure}_bench" extra
	[[ -x "$bin" ]] || { echo "Missing $bin: run experiments/competitors/stxxl/build.sh first" >&2; exit 1; }
	if [[ "$structure" == map ]]; then extra="$MAP_ARGS"; else extra="$UM_ARGS"; fi
	if [[ "$DROP_CACHES" == 1 ]]; then sync; echo 3 > /proc/sys/vm/drop_caches; fi
	local cmd=("$bin" $extra "$@")
	if [[ -n "$MEMORY_MAX" ]]; then
		cmd=(systemd-run --user --scope --quiet -p MemoryMax="$MEMORY_MAX" -p MemorySwapMax=0 "${cmd[@]}")
	fi
	echo "+ ${cmd[*]}"
	(cd "$OUT" && "${cmd[@]}")
}

for structure in $STRUCTURES; do
	for size in $SIZES; do
		for kd in $KEY_DISTS; do
			for ((r = 1; r <= REPEAT; ++r)); do
				run "$structure" --size "$size" --queries "$QUERIES" --key-dist "$kd" --query-dist "$QUERY_DISTS"
			done
		done
		for pair in $INTERLEAVED; do
			for ((r = 1; r <= REPEAT; ++r)); do
				run "$structure" --interleaved --size "$size" --insertions-per-batch "${pair%%:*}" --queries-per-batch "${pair##*:}"
			done
		done
	done
done
