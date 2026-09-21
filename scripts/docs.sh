#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Denis Tishkov <denis8825@ya.ru>
# Generate the Doxygen HTML reference into build/docs/html (requires doxygen).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/docs
doxygen Doxyfile
echo "open build/docs/html/index.html"
