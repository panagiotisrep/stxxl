#!/usr/bin/env bash
# Downloads (git submodules, pinned commits) and builds every competitor, each with its own
# <competitor>/build.sh, into build-competitors/<competitor>/ (stxxl: this repository's STXXL map and unordered_map).
#
# Usage: experiments/competitors/build_all.sh
#   COMPETITORS  which ones (default: "leveldb rocksdb pachash leanstore stxxl")
#   JOBS         parallel compile jobs per build (default: all cores; RocksDB needs ~1 GB RAM per job)
#   PACHASH_BLOCK_LENGTH, PACHASH_WITH_URING   passed on to pachash/build.sh (see pachash/README.md)
#
# Needs git, CMake ≥ 3.25 and a C++20 compiler. LeanStore also needs the system packages
# libtbb-dev and libaio-dev (Ubuntu/Debian: sudo apt-get install libtbb-dev libaio-dev).
# A failing build does not stop the others; the summary at the end lists each result.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
COMPETITORS="${COMPETITORS:-leveldb rocksdb pachash leanstore stxxl}"
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
