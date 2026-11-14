#!/usr/bin/env bash
# Runs BOA (all implementations/modes) and every competitor on the same workload, one after the other,
# and collects all logs in one results folder.
#
# Which runs, with which binary and options, comes from a parameter file, one run per line
# (format and placeholders explained at the top of the file; edit it to change any parameter):
#   FAIR=1 (default)  experiments/params_fair.txt     equal resources (I/O model), see
#                     experiments/competitors/README.md, "Fair comparison"
#   FAIR=0            experiments/params_default.txt  every system with its own defaults, no memory limit
#   PARAMS=<file>     any other file in the same format
#
# FAIR=1: every competitor gets BOA's RAM budget
#   M = (lambda + 3) * Pages * PageSize * BlockSize   (BOA's memory in theory; M_MODE=formula, default)
#       lambda, Pages, PageSize, BlockSize are those of BOA_BIN, read from a tiny calibration run.
#       M_MODE=measured: max "Max internal memory bytes" of the boa runs + one block (the boa runs must
#       come first). M=<bytes> sets it directly.
# The {M...} placeholders of the parameter file give M to each competitor's memory options (caches,
# buffers; params_fair.txt also cuts the buffers outside them, see there). Nothing is killed for using
# more: every competitor's memory is measured and compared with its budget, and summary.txt notes it.
#   structure memory = "Peak RSS above baseline" in its log: peak RSS minus the RSS once its keys are
#                      generated, before the structure exists (the key set is benchmark memory, the same
#                      in boa and every driver, so it is not part of M)
#   budget           = M + overhead + LIMIT_SLACK_MB
#     overhead       the competitor's fixed memory (code, libraries, threads, allocator): "Peak RSS above
#                    baseline" of a calibration run of the same line with CAL_SIZE keys
# A competitor over its budget is marked "over M" in summary.txt with both numbers. BOA's own memory is
# not checked. All runs use the same CPUs (CPUS) and the same workload.
#
# Usage: experiments/run_all.sh
# Settings (environment variables):
#   SIZE         keys to insert                                   (default: 20000000)
#   QUERIES      queries per query distribution                   (default: 20000)
#   KEY_DIST     key distribution                                 (default: uniform)
#   QUERY_DISTS  query distributions of the insert-then-query runs (default: all)
#   BATCH_INSERTS, BATCH_QUERIES  batch sizes of the interleaved runs, {BATCH_INSERTS}/{BATCH_QUERIES}
#                in the parameter file                            (default: 50000, 2000)
#   FAIR         1 = equal-resources run (default), 0 = defaults
#   PARAMS       parameter file (default: by FAIR, see above)
#   RUNS         which runs of the parameter file, in this order (default: all, in file order)
#   CPUS         taskset CPU list for every run                   (default: 0,1)
#   M_MODE       formula (default) or measured, see above;  M  bytes, overrides M_MODE
#   LIMIT_SLACK_MB  added to every competitor's memory budget      (default: 4)
#   CAL_SIZE     keys of the calibration runs (BOA parameters, competitor overhead) (default: 1000)
#   BLOCK        I/O block size, {BLOCK} in the parameter file   (default: 32768 = BOA_BLOCK_SIZE)
#   OUT          results folder (logs only; small)                (default: results/run_<SIZE>_<date>)
#   DATA_DIR     where the data structures put their files: a local file system with O_DIRECT
#                support (not /mnt/c under WSL, not tmpfs)     (default: /var/tmp/boa_experiments)
#   TIMEOUT      seconds per run; a run that takes longer is stopped and marked TIMEOUT (default: 3600)
#   BOA_BIN      the boa benchmark binary ("boa" in the file)     (default: build/examples/containers/boa)
#   STXXL_CFG    .stxxl disk configuration for boa and the STXXL competitors
#                (default: experiments/stxxl.conf, 60 GiB file in /var/tmp/stxxl, O_DIRECT)
#   COMP_BUILD   competitors' build folder (build_all.sh output)  (default: build-competitors)
#                FAIR=1 expects PaCHash built with PACHASH_BLOCK_LENGTH=$BLOCK and the STXXL competitors
#                with STXXL_UM_SUB_BLOCK_SIZE=$BLOCK, as build_all.sh does with the same BLOCK (checked before
#                the runs start; their block sizes are also logged in environment.txt)
#   <NAME>_ARGS  extra options appended for one run, e.g. ROCKSDB_ARGS="--bloom-bits 5" (name in upper case)
# BOA compile-time parameters (BOA_BLOCK_SIZE, BOA_LAMBDA, BOA_PAGES) need a rebuild; point BOA_BIN at it
# (boa.md, "Compile-time macros"). The values of the binary used are logged in params.txt.
#
# Output in OUT: <run>.txt (the structure's results file), <run>.out (console output + /usr/bin/time -v),
# summary.txt (status, wall time, peak RSS per run), environment.txt (machine, versions, commits, config),
# params.txt (M, every competitor's memory limit and its parts, the exact command of every run), params_file.txt (copy of the parameter file),
# table.txt (per-phase time, MB read/written, reads per operation; made by experiments/summarize.py).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE="${SIZE:-20000000}"
QUERIES="${QUERIES:-20000}"
KEY_DIST="${KEY_DIST:-uniform}"
QUERY_DISTS="${QUERY_DISTS:-all}"
BATCH_INSERTS="${BATCH_INSERTS:-50000}"
BATCH_QUERIES="${BATCH_QUERIES:-2000}"
FAIR="${FAIR:-1}"
CPUS="${CPUS:-0,1}"
M_MODE="${M_MODE:-formula}"
LIMIT_SLACK_MB="${LIMIT_SLACK_MB:-4}"
CAL_SIZE="${CAL_SIZE:-1000}"
BLOCK="${BLOCK:-32768}"
OUT="${OUT:-$ROOT/results/run_${SIZE}_$(date +%Y%m%d_%H%M%S)}"
DATA_DIR="${DATA_DIR:-/var/tmp/boa_experiments}"
TIMEOUT="${TIMEOUT:-3600}"
BOA_BIN="${BOA_BIN:-$ROOT/build/examples/containers/boa}"
COMP_BUILD="${COMP_BUILD:-$ROOT/build-competitors}"
STXXL_CFG="${STXXL_CFG:-$ROOT/experiments/stxxl.conf}"

mkdir -p "$OUT" "$DATA_DIR"
OUT="$(cd "$OUT" && pwd)"
WORKLOAD=(--size "$SIZE" --queries "$QUERIES" --key-dist "$KEY_DIST" --query-dist "$QUERY_DISTS")

PARAMS="${PARAMS:-$ROOT/experiments/$([[ "$FAIR" == 1 ]] && echo params_fair.txt || echo params_default.txt)}"
[[ -f "$PARAMS" ]] || { echo "Parameter file not found: $PARAMS" >&2; exit 1; }

# Parameter file: "<run name> <binary> <options...>" per line, # comments. Read into arrays, file order.
declare -a RUN_NAMES=()
declare -A RUN_BIN=() RUN_OPTS=()
while IFS= read -r line || [[ -n "$line" ]]; do
	line="${line%%#*}"
	read -r name bin opts <<< "$line"
	[[ -z "${name:-}" ]] && continue
	[[ -z "${bin:-}" ]] && { echo "$PARAMS: no binary for run '$name'" >&2; exit 1; }
	[[ -n "${RUN_BIN[$name]:-}" ]] && { echo "$PARAMS: run '$name' appears twice" >&2; exit 1; }
	RUN_NAMES+=("$name")
	RUN_BIN[$name]="$bin"
	RUN_OPTS[$name]="${opts:-}"
done < "$PARAMS"
RUNS="${RUNS:-${RUN_NAMES[*]}}"
for run in $RUNS; do
	[[ -n "${RUN_BIN[$run]:-}" ]] || { echo "Run '$run' is not in $PARAMS" >&2; exit 1; }
done

binary() {
	local b="${RUN_BIN[$1]}"
	if [[ "$b" == boa ]]; then echo "$BOA_BIN"; else echo "$COMP_BUILD/$b"; fi
}
is_boa() { [[ "${RUN_BIN[$1]}" == boa ]]; }

M="${M:-0}"
M_FIXED=$([[ "$M" != 0 ]] && echo 1 || echo 0)
# Fills the {...} placeholders of a run's options (integer arithmetic over M, BLOCK, SIZE, QUERIES,
# BATCH_INSERTS, BATCH_QUERIES; {M_GIB}).
expand_options() {
	local opts="$1" out="" expr value
	while [[ "$opts" =~ \{([^}]*)\} ]]; do
		expr="${BASH_REMATCH[1]}"
		if [[ "$expr" =~ (^|[^A-Za-z_])M([^A-Za-z_]|$) || "$expr" == M_GIB ]] && [[ "$M" == 0 ]]; then
			echo "Run options use M ({$expr}), which is only known with FAIR=1 after the boa runs" >&2
			return 1
		fi
		if [[ "$expr" == M_GIB ]]; then
			value="$(awk -v m="$M" 'BEGIN {printf "%.6f", m / 1073741824}')"
		else
			value=$(M=$M BLOCK=$BLOCK SIZE=$SIZE QUERIES=$QUERIES BATCH_INSERTS=$BATCH_INSERTS BATCH_QUERIES=$BATCH_QUERIES; echo $(( expr ))) || return 1
		fi
		out+="${opts%%"{$expr}"*}$value"
		opts="${opts#*"{$expr}"}"
	done
	echo "$out$opts"
}

# FAIR=1: competitors whose block size is fixed at build time must have been built with BLOCK
# (build_all.sh BLOCK=...). Their --help reports it.
if [[ "$FAIR" == 1 ]]; then
	for run in $RUNS; do
		case "${RUN_BIN[$run]}" in
			pachash/*|stxxl/*) ;;
			*) continue ;;
		esac
		bin="$(binary "$run")"
		[[ -x "$bin" ]] || continue
		sizes="$("$bin" --help 2>/dev/null | grep -iE 'block size|block sizes' | grep -oE '(sub-block|node|leaf|Block size \(compile time\):) [0-9]+' | grep -oE '[0-9]+$' | sort -u)"
		if [[ -n "$sizes" && "$sizes" != "$BLOCK" ]]; then
			echo "$run: built with block size(s) $(echo $sizes), but BLOCK=$BLOCK. Rebuild with" >&2
			echo "  BLOCK=$BLOCK experiments/competitors/build_all.sh   (or run with FAIR=0 / BLOCK=<built size>)" >&2
			exit 1
		fi
	done
fi

# BOA's compile-time parameters (from the log of a tiny run): sets BOA_LAMBDA_V, BOA_PAGES_V,
# BOA_PAGE_SIZE_V, BOA_BLOCK_V.
boa_parameters() {
	local d="$DATA_DIR/calibration_boa"
	rm -rf -- "$d"; mkdir -p "$d"
	[[ -f "$STXXL_CFG" ]] && cp "$STXXL_CFG" "$d/.stxxl"
	(cd "$d" && "$BOA_BIN" --size "$CAL_SIZE" --queries 10 --log-file "$d/boa.txt" > /dev/null 2>&1)
	BOA_LAMBDA_V=$(awk '/^Lambda / {print $2; exit}' "$d/boa.txt" 2>/dev/null)
	BOA_PAGES_V=$(awk '/^Pages / {print $2; exit}' "$d/boa.txt" 2>/dev/null)
	BOA_PAGE_SIZE_V=$(awk '/^Page size / {print $3; exit}' "$d/boa.txt" 2>/dev/null)
	BOA_BLOCK_V=$(awk '/^Block size / {print $3; exit}' "$d/boa.txt" 2>/dev/null)
	rm -rf -- "$d"
	[[ -n "$BOA_LAMBDA_V" && -n "$BOA_PAGES_V" && -n "$BOA_PAGE_SIZE_V" && -n "$BOA_BLOCK_V" ]]
}

# A competitor's fixed memory: "Peak RSS above baseline bytes" of the same command with CAL_SIZE keys and
# no limit. Prints the bytes (0 if unknown).
calibrate_overhead() {
	local run="$1" bin="$2"; shift 2
	local d="$DATA_DIR/calibration_$run"
	rm -rf -- "$d"; mkdir -p "$d"
	[[ -f "$STXXL_CFG" ]] && cp "$STXXL_CFG" "$d/.stxxl"
	(cd "$d" && taskset -c "$CPUS" timeout "$TIMEOUT" "$bin" "$@" --size "$CAL_SIZE" --queries 100 \
		--query-dist uniform --key-dist "$KEY_DIST" --log-file "$d/cal.txt" > /dev/null 2>&1)
	local o
	o=$(awk '/^Peak RSS above baseline bytes:/ {print $NF}' "$d/cal.txt" 2>/dev/null | tail -1)
	rm -rf -- "$d"
	echo "${o:-0}"
}

peak_rss_kb() { awk -F: '/Maximum resident set size/ {gsub(/ /, "", $2); print $2}' "$1" 2>/dev/null | tail -1; }

{
	echo "Date: $(date -Iseconds)"
	echo "Host: $(uname -a)"
	echo "CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //') ($(nproc) threads); runs pinned to CPUs $CPUS"
	echo "Memory: $(free -b | awk '/Mem:/ {print $2}') bytes"
	echo "Compiler: $(c++ --version | head -1)"
	echo "Data dir: $DATA_DIR ($(df -T "$DATA_DIR" | awk 'NR==2 {print $2}'))"
	echo "Repository: $(git -C "$ROOT" rev-parse HEAD) $(git -C "$ROOT" diff --quiet 2>/dev/null && git -C "$ROOT" diff --cached --quiet 2>/dev/null || echo '(uncommitted changes)')"
	echo "Submodules:"; git -C "$ROOT" submodule status 2>/dev/null | sed 's/^/  /'
	echo "Workload: ${WORKLOAD[*]}; interleaved runs: batches of $BATCH_INSERTS insertions + $BATCH_QUERIES queries"
	echo "Mode: FAIR=$FAIR, block $BLOCK, timeout per run $TIMEOUT s"
	echo "boa binary: $BOA_BIN"
	echo "Competitors build: $COMP_BUILD"
	if [[ -f "$STXXL_CFG" ]]; then echo "STXXL config ($STXXL_CFG):"; awk '{print "  " $0}' "$STXXL_CFG"; else echo "STXXL config: STXXL default"; fi
	if [[ "$FAIR" == 1 ]]; then
		echo "PaCHash: $("$COMP_BUILD/pachash/pachash_bench" --help 2>/dev/null | grep 'Block size')"
		echo "STXXL unordered_map: $("$COMP_BUILD/stxxl/stxxl_unordered_map_bench" --help 2>/dev/null | grep 'Block sizes')"
	fi
} > "$OUT/environment.txt"

printf '%-22s %-13s %8s %12s  %s\n' run status seconds peak_rss_MB "structure/budget_MB (competitors: peak RSS above key set / M + fixed overhead + slack)" > "$OUT/summary.txt"
: > "$OUT/params.txt"
cp "$PARAMS" "$OUT/params_file.txt"
echo "Parameter file: $PARAMS (copied as params_file.txt)" >> "$OUT/params.txt"
if [[ "$FAIR" == 1 && "$M_FIXED" == 1 ]]; then
	echo "M = $M bytes (set with M=)" >> "$OUT/params.txt"
elif [[ "$FAIR" == 1 && "$M_MODE" == formula ]]; then
	boa_parameters || { echo "Could not read lambda/Pages/Block size from a calibration run of $BOA_BIN" >&2; exit 1; }
	M=$(( (BOA_LAMBDA_V + 3) * BOA_PAGES_V * BOA_PAGE_SIZE_V * BOA_BLOCK_V ))
	echo "M = $M bytes = (lambda $BOA_LAMBDA_V + 3) * Pages $BOA_PAGES_V * PageSize $BOA_PAGE_SIZE_V * Block $BOA_BLOCK_V (M_MODE=formula)" >> "$OUT/params.txt"
	[[ "$BOA_BLOCK_V" == "$BLOCK" ]] || echo "Warning: BOA block size $BOA_BLOCK_V differs from BLOCK=$BLOCK" >&2
fi
for run in $RUNS; do
	bin="$(binary "$run")"
	var="$(echo "${run}_ARGS" | tr '[:lower:]' '[:upper:]')"
	extra="${!var:-}"
	if [[ ! -x "$bin" ]]; then
		printf '%-22s %-13s %8s %12s\n' "$run" "NO-BINARY" "-" "-" | tee -a "$OUT/summary.txt"
		continue
	fi

	# FAIR=1, M_MODE=measured: after the boa runs, derive M from them (once).
	if [[ "$FAIR" == 1 && "$M" == 0 && "$M_MODE" == measured ]] && ! is_boa "$run"; then
		boa_mem=$(grep -h "^Max internal memory bytes:" "$OUT"/boa*.txt 2>/dev/null | awk '{print $NF}' | sort -n | tail -1)
		[[ -n "$boa_mem" ]] || { echo "M_MODE=measured needs the boa runs first (put them first in $PARAMS / RUNS)" >&2; exit 1; }
		M=$((boa_mem + BLOCK))
		echo "M = $M bytes (M_MODE=measured: max BOA 'Max internal memory bytes' $boa_mem + one block $BLOCK)" >> "$OUT/params.txt"
	fi
	limited=0
	[[ "$FAIR" == 1 ]] && ! is_boa "$run" && limited=1
	opts="$(expand_options "${RUN_OPTS[$run]}")" || exit 1

	cwd="$DATA_DIR/$run" # fresh working directory per run: the structure's files go there
	rm -rf -- "$cwd"
	mkdir -p "$cwd"
	[[ -f "$STXXL_CFG" ]] && cp "$STXXL_CFG" "$cwd/.stxxl"
	# shellcheck disable=SC2206
	cmd=("$bin" $opts $extra "${WORKLOAD[@]}" --log-file "$OUT/$run.txt")
	overhead=0
	[[ "$limited" == 1 ]] && overhead=$(calibrate_overhead "$run" "$bin" $opts $extra)
	full=(taskset -c "$CPUS" /usr/bin/time -v timeout "$TIMEOUT" "${cmd[@]}")
	echo "$run: ${full[*]}" >> "$OUT/params.txt"
	echo "=== $run"
	echo "${full[*]}" > "$OUT/$run.out"
	start=$SECONDS
	(cd "$cwd" && "${full[@]}") >> "$OUT/$run.out" 2>&1
	rc=$?
	case $rc in
		0) status=ok ;;
		124) status=TIMEOUT ;;
		137) status=KILLED/OOM ;;
		*) status="FAILED($rc)" ;;
	esac
	grep -q "did not find\|wrong result\|query error" "$OUT/$run.txt" 2>/dev/null && status="LOOKUP-ERROR"
	rss=$(peak_rss_kb "$OUT/$run.out")
	mem="-"
	if [[ "$limited" == 1 ]]; then
		used=$(awk '/^Peak RSS above baseline bytes:/ {print $NF}' "$OUT/$run.txt" 2>/dev/null | tail -1)
		budget=$((M + overhead + LIMIT_SLACK_MB * 1048576))
		if [[ -n "$used" ]]; then
			mem=$(awk -v u="$used" -v b="$budget" 'BEGIN {printf "%.1f/%.1f%s", u/1048576, b/1048576, (u > b ? " over M" : "")}')
		fi
		echo "  $run memory: structure ${used:-?} bytes (peak RSS above baseline), budget $budget = M $M + overhead $overhead + slack $((LIMIT_SLACK_MB * 1048576))$([[ -n "$used" && "$used" -gt "$budget" ]] && echo ' -> OVER M')" >> "$OUT/params.txt"
	fi
	printf '%-22s %-13s %8s %12s  %s\n' "$run" "$status" "$((SECONDS - start))" "$(( ${rss:-0} / 1024 ))" "$mem" | tee -a "$OUT/summary.txt"
	rm -rf -- "$cwd"
done
for f in "$OUT"/boa*.txt; do # compile-time parameters of the BOA binary, as it logged them
	[[ -f "$f" ]] || continue
	echo "BOA compile time (from $(basename "$f")): $(grep -m3 -E '^(Lambda|Pages|Block size) ' "$f" | paste -sd, | sed 's/,/, /g')" >> "$OUT/params.txt"
	break
done
python3 "$ROOT/experiments/summarize.py" "$OUT" > "$OUT/table.txt" || echo "summarize.py failed" >&2
echo "Results: $OUT"
