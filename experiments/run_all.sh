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
# FAIR=1: the boa runs go first; from their logs the script derives
#   M          internal memory budget = max over the boa runs of "Max internal memory bytes"
#              + one block (BOA's insertion buffer); the {M...} placeholders of the other runs use it
#   MEMLIMIT   cgroup memory limit for every other run = max BOA peak RSS + SLACK_MB
#              (covers the benchmark's own key/query arrays, the same in every driver)
# and runs every other line under MEMLIMIT. All runs use the same CPUs (CPUS) and the same workload.
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
#   SLACK_MB     added to BOA's peak RSS for MEMLIMIT             (default: 64)
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
# params.txt (M, MEMLIMIT and the exact command of every run), params_file.txt (copy of the parameter file),
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
SLACK_MB="${SLACK_MB:-64}"
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

M=0
MEMLIMIT_MB=0
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

printf '%-22s %-13s %8s %12s\n' run status seconds peak_rss_MB > "$OUT/summary.txt"
: > "$OUT/params.txt"
cp "$PARAMS" "$OUT/params_file.txt"
echo "Parameter file: $PARAMS (copied as params_file.txt)" >> "$OUT/params.txt"
for run in $RUNS; do
	bin="$(binary "$run")"
	var="$(echo "${run}_ARGS" | tr '[:lower:]' '[:upper:]')"
	extra="${!var:-}"
	if [[ ! -x "$bin" ]]; then
		printf '%-22s %-13s %8s %12s\n' "$run" "NO-BINARY" "-" "-" | tee -a "$OUT/summary.txt"
		continue
	fi

	# FAIR=1: after the BOA runs, derive M and MEMLIMIT from them (once).
	limit=()
	unit=""
	if [[ "$FAIR" == 1 ]] && ! is_boa "$run"; then
		if [[ "$M" == 0 ]]; then
			boa_mem=$(grep -h "^Max internal memory bytes:" "$OUT"/boa*.txt 2>/dev/null | awk '{print $NF}' | sort -n | tail -1)
			boa_rss=$(for f in "$OUT"/boa*.out; do peak_rss_kb "$f"; done | sort -n | tail -1)
			if [[ -z "$boa_mem" || -z "$boa_rss" ]]; then
				echo "FAIR=1 needs the boa runs first (they measure M; put them first in $PARAMS / RUNS)" >&2; exit 1
			fi
			M=$((boa_mem + BLOCK))
			MEMLIMIT_MB=$((boa_rss / 1024 + SLACK_MB))
			{
				echo "M = $M bytes (max BOA 'Max internal memory bytes' $boa_mem + one block $BLOCK)"
				echo "MEMLIMIT = ${MEMLIMIT_MB} MB (max BOA peak RSS $((boa_rss / 1024)) MB + $SLACK_MB MB)"
				echo
			} >> "$OUT/params.txt"
		fi
		unit="boa-bench-$run-$$"
		limit=(systemd-run --user --scope --quiet --unit="$unit" -p MemoryMax="${MEMLIMIT_MB}M" -p MemorySwapMax=0)
	fi
	opts="$(expand_options "${RUN_OPTS[$run]}")" || exit 1

	cwd="$DATA_DIR/$run" # fresh working directory per run: the structure's files go there
	rm -rf -- "$cwd"
	mkdir -p "$cwd"
	[[ -f "$STXXL_CFG" ]] && cp "$STXXL_CFG" "$cwd/.stxxl"
	# shellcheck disable=SC2206
	cmd=("$bin" $opts $extra "${WORKLOAD[@]}" --log-file "$OUT/$run.txt")
	full=("${limit[@]}" taskset -c "$CPUS" /usr/bin/time -v timeout "$TIMEOUT" "${cmd[@]}")
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
	# The cgroup memory limit kills the scope (the exit code is then 137 or 143): systemd records it.
	if [[ -n "$unit" && $rc != 0 ]]; then
		[[ "$(systemctl --user show -p Result --value "$unit.scope" 2>/dev/null)" == oom-kill ]] && status=OOM
		systemctl --user reset-failed "$unit.scope" 2>/dev/null
	fi
	grep -q "did not find\|wrong result\|query error" "$OUT/$run.txt" 2>/dev/null && status="LOOKUP-ERROR"
	rss=$(peak_rss_kb "$OUT/$run.out")
	printf '%-22s %-13s %8s %12s\n' "$run" "$status" "$((SECONDS - start))" "$(( ${rss:-0} / 1024 ))" | tee -a "$OUT/summary.txt"
	rm -rf -- "$cwd"
done
for f in "$OUT"/boa*.txt; do # compile-time parameters of the BOA binary, as it logged them
	[[ -f "$f" ]] || continue
	echo "BOA compile time (from $(basename "$f")): $(grep -m3 -E '^(Lambda|Pages|Block size) ' "$f" | paste -sd, | sed 's/,/, /g')" >> "$OUT/params.txt"
	break
done
python3 "$ROOT/experiments/summarize.py" "$OUT" > "$OUT/table.txt" || echo "summarize.py failed" >&2
echo "Results: $OUT"
