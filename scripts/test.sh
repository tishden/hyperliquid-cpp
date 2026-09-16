#!/usr/bin/env bash
# Build a preset and run the full test suite (unit + integration against the in-process mock venue).
#   scripts/test.sh [release|asan] [gtest filter]
set -euo pipefail
cd "$(dirname "$0")/.."
preset="${1:-release}"
filter="${2:-*}"
scripts/build.sh "$preset"
"build/$preset/tests/hl_tests" --gtest_filter="$filter"
