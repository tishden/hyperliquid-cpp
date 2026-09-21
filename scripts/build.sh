#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Denis Tishkov <denis8825@ya.ru>
# Configure and build a preset: release (default), debug or asan.
#   scripts/build.sh [release|debug|asan]
set -euo pipefail
cd "$(dirname "$0")/.."
preset="${1:-release}"
cmake --preset "$preset"
cmake --build --preset "$preset" -j"$(nproc)"
