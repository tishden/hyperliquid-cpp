# SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
# Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
add_executable(hl_benchmarks
    bench_parser.cpp
    bench_order_book.cpp
    bench_decimal.cpp
    bench_signing.cpp
    bench_ws_codec.cpp)
target_link_libraries(hl_benchmarks PRIVATE hyperliquid benchmark::benchmark benchmark::benchmark_main)
target_compile_definitions(hl_benchmarks PRIVATE HL_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
target_include_directories(hl_benchmarks PRIVATE ${CMAKE_SOURCE_DIR}/tests)
hl_target_defaults(hl_benchmarks)
