#!/usr/bin/env bash
# Generate the Doxygen HTML reference into build/docs/html (requires doxygen).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/docs
doxygen Doxyfile
echo "open build/docs/html/index.html"
