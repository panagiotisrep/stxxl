#!/usr/bin/env bash
# Runs silt_bench under a cgroup memory limit (anonymous + page-cache memory; no swap).
# usage: silt_limited.sh --memory-max 8G <silt_bench options>
# If SILT needs more than the limit for its anonymous memory it is killed by the OOM killer (the run shows as FAILED).
set -euo pipefail
[[ "${1:-}" == --memory-max ]] || { echo "usage: $0 --memory-max SIZE <silt_bench options>" >&2; exit 1; }
LIMIT="$2"; shift 2
BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/build-competitors/silt/silt_bench"
exec systemd-run --user --scope --quiet -p MemoryMax="$LIMIT" -p MemorySwapMax=0 "$BIN" "$@"
