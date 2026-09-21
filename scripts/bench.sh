#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Denis Tishkov <denis8825@ya.ru>
# Build the release preset and run the benchmark suite pinned to one core.
#   scripts/bench.sh [cpu core, default 2] [extra google-benchmark args...]
set -euo pipefail
cd "$(dirname "$0")/.."
core="${1:-2}"
shift || true
scripts/build.sh release
taskset -c "$core" build/release/benchmarks/hl_benchmarks --benchmark_min_time=0.5s "$@"
