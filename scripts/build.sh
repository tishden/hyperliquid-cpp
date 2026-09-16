#!/usr/bin/env bash
# Configure and build a preset: release (default), debug or asan.
#   scripts/build.sh [release|debug|asan]
set -euo pipefail
cd "$(dirname "$0")/.."
preset="${1:-release}"
cmake --preset "$preset"
cmake --build --preset "$preset" -j"$(nproc)"
