# syntax=docker/dockerfile:1
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#
# Multi-stage image for hyperliquid-cpp.
#
#   docker build -t hyperliquid-cpp .                      # build + run the test suite + runtime image
#   docker build --target dev -t hyperliquid-cpp:dev .     # full toolchain with the built tree
#
#   docker run --rm hyperliquid-cpp hl_book_printer BTC ETH
#   docker run --rm hyperliquid-cpp hl_testnet_quoter --dry-run --coin ETH
#   docker run --rm -e HL_PRIVATE_KEY -e HL_ACCOUNT_ADDRESS hyperliquid-cpp hl_testnet_quoter --coin ETH
#   docker run --rm hyperliquid-cpp hl_benchmarks

ARG UBUNTU_VERSION=24.04

# ── build: compile, test, install ────────────────────────────────────────────
FROM ubuntu:${UBUNTU_VERSION} AS build
ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_TYPE=Release
ARG RUN_TESTS=ON
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates pkg-config \
        libssl-dev libgtest-dev libbenchmark-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    && cmake --build build -j"$(nproc)" \
    && if [ "${RUN_TESTS}" = "ON" ]; then build/tests/hl_tests --gtest_brief=1; fi \
    && cmake --install build --prefix /opt/hyperliquid-cpp \
    && mkdir -p /opt/hyperliquid-cpp/bin \
    && install -m 0755 build/examples/hl_testnet_quoter build/examples/hl_book_printer build/examples/hl_live_check \
                       build/benchmarks/hl_benchmarks build/tests/hl_tests /opt/hyperliquid-cpp/bin/

# ── dev: toolchain + built tree (for developing against the library) ────────
FROM build AS dev
ENV PATH=/opt/hyperliquid-cpp/bin:${PATH}
CMD ["bash"]

# ── runtime: binaries only ───────────────────────────────────────────────────
FROM ubuntu:${UBUNTU_VERSION} AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libssl3t64 libbenchmark1.8.3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 trader
COPY --from=build /opt/hyperliquid-cpp/bin/ /usr/local/bin/
COPY --from=build /src/LICENSE /src/NOTICE /src/THIRD_PARTY_NOTICES.md /usr/share/doc/hyperliquid-cpp/
# test/benchmark fixtures at the path compiled into hl_tests / hl_benchmarks
COPY --from=build /src/tests/fixtures /src/tests/fixtures
ENV SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
USER trader
WORKDIR /home/trader
CMD ["hl_testnet_quoter", "--help"]
