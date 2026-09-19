#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
# Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
# Build a preset and run the full test suite (unit + integration against the in-process mock venue).
#   scripts/test.sh [release|asan] [gtest filter]
set -euo pipefail
cd "$(dirname "$0")/.."
preset="${1:-release}"
filter="${2:-*}"
scripts/build.sh "$preset"
"build/$preset/tests/hl_tests" --gtest_filter="$filter"
