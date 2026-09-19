# SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
# Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
cmake_minimum_required(VERSION 3.21)

project(hyperliquid-cpp
    VERSION 1.4.1
    DESCRIPTION "Low-latency C++ connector for Hyperliquid: market data, order management, EIP-712 signing"
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Tests, benchmarks, examples and -Werror default to ON only for a top-level build,
# so add_subdirectory()/FetchContent consumers get just the library.
option(HL_BUILD_TESTS        "Build unit and integration tests"  ${PROJECT_IS_TOP_LEVEL})
option(HL_BUILD_BENCHMARKS   "Build Google Benchmark suite"      ${PROJECT_IS_TOP_LEVEL})
option(HL_BUILD_EXAMPLES     "Build example applications"        ${PROJECT_IS_TOP_LEVEL})
option(HL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(HL_SANITIZE         "Build with AddressSanitizer + UBSan"      OFF)
option(HL_NATIVE           "Optimise for the build machine (-march=native); do not use for portable binaries" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(Dependencies)

# ── Library ─────────────────────────────────────────────────────────────────
add_library(hyperliquid
    src/core/Decimal.cpp
    src/core/Hex.cpp
    src/core/Log.cpp
    src/crypto/Keccak.cpp
    src/crypto/Signer.cpp
    src/om/Actions.cpp
    src/om/AssetRegistry.cpp
    src/om/ExchangeResponse.cpp
    src/om/ExchangeClient.cpp
    src/om/InfoClient.cpp
    src/md/OrderBook.cpp
    src/md/MarketDataClient.cpp
    src/net/EventLoop.cpp
    src/net/TlsStream.cpp
    src/net/Url.cpp
    src/net/WsCodec.cpp
    src/net/WebSocketClient.cpp
    src/net/WsSession.cpp
    src/net/HttpCodec.cpp
    src/net/HttpClient.cpp
    src/WsMessageParser.cpp
)
add_library(hyperliquid::hyperliquid ALIAS hyperliquid)

target_include_directories(hyperliquid
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Every third-party dependency is PRIVATE: no public header includes one, they appear in the API
# only as opaque forward declarations (`struct ssl_ctx_st`, `struct secp256k1_context_struct`).
# Consumers still link them transitively (CMake keeps PRIVATE deps of a static library as
# LINK_ONLY), but they do not inherit the include paths and cannot accidentally compile against
# a version of OpenSSL or simdjson the library was not built with.
target_link_libraries(hyperliquid
    PRIVATE OpenSSL::SSL OpenSSL::Crypto hl_secp256k1 hl_simdjson)

set(HL_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
                     -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    # GCC 11/12 emit false-positive -Wstringop-overread from libstdc++'s string_view::find at -O3.
    list(APPEND HL_WARNING_FLAGS -Wno-stringop-overread)
endif()
if(HL_WARNINGS_AS_ERRORS)
    list(APPEND HL_WARNING_FLAGS -Werror)
endif()

function(hl_target_defaults target)
    target_compile_options(${target} PRIVATE ${HL_WARNING_FLAGS})
    if(HL_NATIVE)
        target_compile_options(${target} PRIVATE -march=native)
    endif()
    if(HL_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

hl_target_defaults(hyperliquid)
if(HL_SANITIZE)
    target_link_options(hyperliquid PUBLIC -fsanitize=address,undefined)
endif()

# ── Sub-projects ─────────────────────────────────────────────────────────────
if(HL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(HL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(HL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
# Installs the public headers and the static library together with the two
# statically built private dependencies it needs at link time.
include(GNUInstallDirs)
install(TARGETS hyperliquid ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES $<TARGET_FILE:secp256k1> $<TARGET_FILE:simdjson> DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/hl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
