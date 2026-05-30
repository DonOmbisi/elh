#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$ELH_DIR/.." && pwd)"
BUILD_DIR="$ELH_DIR/build"
DATA_DIR="${1:-$ROOT_DIR/silesia}"
RESULTS_DIR="${2:-$ROOT_DIR/elh_test_results}"

mkdir -p "$RESULTS_DIR"

if [[ ! -x "$BUILD_DIR/elh_bench" ]]; then
  echo "Missing benchmark binary: $BUILD_DIR/elh_bench" >&2
  echo "Run: cmake --build $BUILD_DIR" >&2
  exit 1
fi

if [[ ! -d "$DATA_DIR" ]]; then
  echo "Missing data directory: $DATA_DIR" >&2
  exit 1
fi

out="$RESULTS_DIR/elh_silesia_$(date +%Y%m%d_%H%M%S).txt"

for f in "$DATA_DIR"/*; do
  [[ -f "$f" ]] || continue
  {
    echo "===== $(basename "$f") ====="
    "$BUILD_DIR/elh_bench" "$f"
    echo
  } | tee -a "$out"
done

echo "Wrote $out"
