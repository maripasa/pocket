#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  ./run.sh <board>

Argument:
  <board>   Target board name passed directly to -DPICO_BOARD
EOF
}

if [[ $# -ne 1 ]]; then
  usage
  exit 1
fi

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 0
fi

BOARD="$1"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
UF2_PATH="${BUILD_DIR}/board.uf2"

rm -rf "${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DPICO_BOARD="${BOARD}"
cmake --build "${BUILD_DIR}"

if [[ ! -f "${UF2_PATH}" ]]; then
  echo "Missing UF2 output: ${UF2_PATH}" >&2
  exit 1
fi

picotool load "${UF2_PATH}" -xv
