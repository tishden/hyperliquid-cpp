# Third-party dependencies.
#
#   OpenSSL >= 3.0      system package (TLS, SHA-1 for the WS handshake, CSPRNG)
#   libsecp256k1 0.6.0  fetched + built static with the recovery module (ECDSA signing)
#   simdjson 3.12.3     fetched + built static (JSON parsing; private to the library)
#   GoogleTest / Google Benchmark — system packages if present, fetched otherwise
#
# The fetched dependencies are linked PRIVATELY: consumers of libhyperliquid
# never see simdjson or secp256k1 headers.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

find_package(OpenSSL 3.0 REQUIRED)

# ── libsecp256k1 ─────────────────────────────────────────────────────────────
set(SECP256K1_ENABLE_MODULE_RECOVERY ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH     OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_MUSIG    OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ELLSWIFT OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS            OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK        OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES         OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS      OFF CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL                OFF CACHE BOOL "" FORCE)
set(SECP256K1_DISABLE_SHARED         ON  CACHE BOOL "" FORCE)
FetchContent_Declare(secp256k1
    GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1.git
    GIT_TAG        v0.6.0
    GIT_SHALLOW    TRUE)
set(_hl_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(secp256k1)
set(BUILD_SHARED_LIBS ${_hl_saved_shared})
add_library(hl_secp256k1 INTERFACE)
target_link_libraries(hl_secp256k1 INTERFACE secp256k1)

# ── simdjson ─────────────────────────────────────────────────────────────────
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
set(SIMDJSON_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.12.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(simdjson)
add_library(hl_simdjson INTERFACE)
target_link_libraries(hl_simdjson INTERFACE simdjson::simdjson)

# ── Test / benchmark frameworks ──────────────────────────────────────────────
if(HL_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.15.2
            GIT_SHALLOW    TRUE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()
if(HL_BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(benchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG        v1.9.1
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(benchmark)
    endif()
endif()
