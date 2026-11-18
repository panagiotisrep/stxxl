#!/usr/bin/env bash
# Runs a competitor binary under a cgroup memory limit (anonymous + page-cache memory, mmapped file pages included; no swap).
# usage: limited.sh --memory-max SIZE <binary relative to build-competitors> <options>
# The limit has to cover the benchmark's key set (SIZE x 4 bytes); if anonymous memory exceeds it, the OOM killer ends the run (shown as FAILED).
set -euo pipefail
[[ "${1:-}" == --memory-max ]] || { echo "usage: $0 --memory-max SIZE <binary> <options>" >&2; exit 1; }
LIMIT="$2"; BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/build-competitors/$3"; shift 3
exec systemd-run --user --scope --quiet -p MemoryMax="$LIMIT" -p MemorySwapMax=0 "$BIN" "$@"
