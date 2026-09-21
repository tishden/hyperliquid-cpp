#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#
# Everything that must be green before a release, in one command.
#
#   scripts/ci.sh              # the default matrix: release, debug, asan, tsan + doc and secret checks
#   scripts/ci.sh --quick      # release only, for a fast pre-commit loop
#   scripts/ci.sh --docker     # also build the Docker image (runs the suite again inside it)
#
# Stages that need a toolchain this machine does not have are skipped, not failed, so the script is
# usable both on a laptop and on a CI runner. Every stage's verdict is printed at the end and the
# exit code is non-zero if any of them failed.
set -uo pipefail
cd "$(dirname "$0")/.."

QUICK=0
DOCKER=0
for arg in "$@"; do
    case "$arg" in
        --quick)  QUICK=1 ;;
        --docker) DOCKER=1 ;;
        -h|--help) sed -n '5,13p' "$0"; exit 0 ;;
        *) echo "unknown option $arg" >&2; exit 2 ;;
    esac
done

RESULTS=()
FAILED=0

# record <name> <status 0|1> [detail]
record() {
    local status=$1
    [ "$status" -ne 0 ] && FAILED=1
    RESULTS+=("$(printf '%-28s %-7s %s' "$2" "$([ "$status" -eq 0 ] && echo PASS || echo FAIL)" "${3:-}")")
}

skip() { RESULTS+=("$(printf '%-28s %-7s %s' "$1" "SKIP" "${2:-}")"); }

stage() { printf '\n\033[1m▶ %s\033[0m\n' "$1"; }

# Configure, build and test one directory. Extra cmake arguments follow the compiler.
build_and_test() {
    local name=$1 dir=$2 compiler=$3; shift 3
    if [ -n "$compiler" ] && ! command -v "$compiler" >/dev/null 2>&1 && [ ! -x "$compiler" ]; then
        skip "$name" "no $compiler"
        return
    fi
    stage "$name"
    local args=(-S . -B "$dir" -GNinja -DCMAKE_BUILD_TYPE=Release -DHL_BUILD_TESTS=ON "$@")
    [ -n "$compiler" ] && args+=(-DCMAKE_CXX_COMPILER="$compiler")
    if ! cmake "${args[@]}" >/dev/null; then
        record 1 "$name" "configure failed"
        return
    fi
    if ! cmake --build "$dir" -j"$(nproc)" >/dev/null; then
        record 1 "$name" "build failed"
        return
    fi
    local out
    out=$(ctest --test-dir "$dir" -j4 --output-on-failure 2>&1)
    local rc=$?
    echo "$out" | tail -3
    record "$rc" "$name" "$(echo "$out" | grep -oE '[0-9]+% tests passed.*' | head -1)"
}

# ── compilers and sanitizers ────────────────────────────────────────────────────────────────────
build_and_test "release (default cxx)" build/ci-release ""

if [ "$QUICK" -eq 0 ]; then
    # A second compiler catches what one frontend accepts and another does not.
    for alt in /opt/rh/gcc-toolset-15/root/usr/bin/g++ g++-15 g++-14 g++-13 clang++-21 clang++; do
        if command -v "$alt" >/dev/null 2>&1 || [ -x "$alt" ]; then
            build_and_test "release (2nd compiler)" build/ci-alt "$alt"
            break
        fi
    done

    # Sanitizers want a compiler whose runtime is actually installed; clang is the reliable one
    # here, and both stages are skipped rather than failed when it is absent. Google Benchmark is
    # not built with a sanitizer, so its targets cannot link and are left out.
    san_cxx=""
    for c in clang++-21 clang++; do
        command -v "$c" >/dev/null 2>&1 && { san_cxx=$c; break; }
    done
    if [ -n "$san_cxx" ]; then
        # On a host with more than one GCC installed, clang can compile against the newest
        # libstdc++ headers while linking the default runtime, which fails on symbols only the
        # newer library defines. Pin it to the GCC the linker will actually use.
        pin=""
        gcc_dir=$(dirname "$(gcc -print-libgcc-file-name 2>/dev/null)" 2>/dev/null) || true
        [ -d "${gcc_dir:-}" ] && pin="--gcc-install-dir=$gcc_dir"

        build_and_test "AddressSanitizer+UBSan" build/ci-asan "$san_cxx" \
            -DHL_SANITIZE=ON -DHL_BUILD_BENCHMARKS=OFF -DCMAKE_CXX_FLAGS="$pin"
        # -fsanitize=thread as a compile flag is enough: clang links its runtime automatically.
        build_and_test "ThreadSanitizer" build/ci-tsan "$san_cxx" \
            -DCMAKE_CXX_FLAGS="-fsanitize=thread $pin" \
            -DHL_BUILD_EXAMPLES=OFF -DHL_BUILD_BENCHMARKS=OFF
    else
        skip "AddressSanitizer+UBSan" "no clang++"
        skip "ThreadSanitizer" "no clang++"
    fi
fi

# ── documentation ───────────────────────────────────────────────────────────────────────────────
stage "documentation links"
if out=$(python3 scripts/check-docs.py); then
    echo "$out" | tail -1
    record 0 "documentation links" "$(echo "$out" | tail -1 | sed 's/check-docs: //')"
else
    echo "$out"
    record 1 "documentation links"
fi

# ── nothing confidential in tracked files ───────────────────────────────────────────────────────
# A key or a wallet address must never reach the repository. Test vectors and captured public
# market data are expected to contain hex, so those paths are exempt.
stage "no secrets in tracked files"
leaks=0
if git ls-files --error-unmatch secrets >/dev/null 2>&1; then
    echo "  secrets/ is tracked by git"
    leaks=1
fi
while IFS= read -r f; do
    case "$f" in tests/*|benchmarks/*|docs/SIGNING.md|credentials.env.example) continue ;; esac
    # 64 hex = a private key, 40 hex = an address; all-zero placeholders are fine.
    if grep -qIE '0x[0-9a-fA-F]{64}' "$f" && ! grep -qIE '0x0{64}' "$f"; then
        echo "  possible private key in $f"
        leaks=1
    fi
    # /home/trader is a path inside the container image, not a developer's machine.
    if grep -hoIE '/home/[a-z][a-z0-9_-]*|/Users/[a-z][a-z0-9_-]*' "$f" | grep -qv '^/home/trader$'; then
        echo "  absolute home path in $f"
        leaks=1
    fi
done < <(git ls-files)
record "$leaks" "no secrets in tracked files"

# ── every source file carries the licence header ──────────────────────────────────────────────
stage "licence headers"
missing=0
while IFS= read -r f; do
    if ! head -5 "$f" | grep -q 'SPDX-License-Identifier: Apache-2.0'; then
        echo "  no SPDX header in $f"
        missing=1
    fi
done < <(git ls-files '*.h' '*.cpp' '*.sh' '*.py' 'CMakeLists.txt' '*.cmake' 'Dockerfile')
record "$missing" "licence headers"

# ── version and changelog agree ─────────────────────────────────────────────────────────────────
stage "version consistency"
ver=$(grep -oE 'kVersionString = "[0-9.]+"' include/hl/Version.h | grep -oE '[0-9.]+')
if grep -q "## \[$ver\]" CHANGELOG.md; then
    record 0 "version consistency" "$ver documented"
else
    echo "  CHANGELOG.md has no section for $ver"
    record 1 "version consistency" "$ver"
fi

# ── docker ──────────────────────────────────────────────────────────────────────────────────────
if [ "$DOCKER" -eq 1 ]; then
    stage "docker image"
    if command -v docker >/dev/null 2>&1; then
        if docker build -t hyperliquid-cpp . >/tmp/hl-docker-ci.log 2>&1; then
            record 0 "docker image" "$(grep -oE '[0-9]+ tests from' /tmp/hl-docker-ci.log | tail -1)"
        else
            tail -20 /tmp/hl-docker-ci.log
            record 1 "docker image"
        fi
    else
        skip "docker image" "docker not installed"
    fi
fi

# ── report ──────────────────────────────────────────────────────────────────────────────────────
printf '\n══ ci summary ═══════════════════════════════════════════\n'
printf '  %s\n' "${RESULTS[@]}"
printf '═════════════════════════════════════════════════════════\n'
exit "$FAILED"
