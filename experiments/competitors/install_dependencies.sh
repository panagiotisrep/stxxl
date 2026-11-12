#!/usr/bin/env bash
# Installs the system packages needed to build boa and every competitor (Ubuntu/Debian, apt).
# The list matches "System packages" in README.md:
#   all (and boa)  build-essential cmake git
#   run_all.sh     time (/usr/bin/time) python3 (summarize.py)
#   LeanStore      libtbb-dev libaio-dev
#   SILT           libxerces-c-dev libssl-dev libgsl-dev libtbb-dev libboost-dev
#   PaCHash        liburing-dev, only with --with-uring (for PACHASH_WITH_URING=ON)
#
# Usage: experiments/competitors/install_dependencies.sh [--with-uring] [--dry-run]
#   --with-uring  also install liburing-dev
#   --dry-run     only show what apt would install (no root needed)
set -euo pipefail

PACKAGES=(build-essential cmake git time python3 libtbb-dev libaio-dev libxerces-c-dev libssl-dev libgsl-dev libboost-dev)
DRY_RUN=0

for arg in "$@"; do
	case "$arg" in
		--with-uring) PACKAGES+=(liburing-dev) ;;
		--dry-run) DRY_RUN=1 ;;
		-h|--help) sed -n '2,12p' "$0"; exit 0 ;;
		*) echo "Unknown option: $arg" >&2; exit 1 ;;
	esac
done

if ! command -v apt-get > /dev/null; then
	echo "apt-get not found: this script supports Ubuntu/Debian only." >&2
	echo "Install the equivalents of these packages with your package manager: ${PACKAGES[*]}" >&2
	exit 1
fi

if [[ "$DRY_RUN" == 1 ]]; then
	apt-get install -s --no-install-recommends "${PACKAGES[@]}" | grep -E '^(Inst|Conf)|newly installed' || true
	exit 0
fi

SUDO=()
if [[ "$(id -u)" != 0 ]]; then
	command -v sudo > /dev/null || { echo "Run as root or install sudo." >&2; exit 1; }
	SUDO=(sudo)
fi

"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y --no-install-recommends "${PACKAGES[@]}"

# CMake ≥ 3.25 is needed (PaCHash). Older distributions ship an older cmake.
cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
if [[ "$(printf '%s\n3.25\n' "$cmake_version" | sort -V | head -1)" != "3.25" ]]; then
	echo "Warning: cmake $cmake_version is older than 3.25, which PaCHash needs." >&2
	echo "Install a newer one, e.g. 'pip install cmake' or https://cmake.org/download/." >&2
fi
echo "Installed: ${PACKAGES[*]}"
