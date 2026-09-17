include(GoogleTest)

add_library(hl_test_support STATIC support/MockVenue.cpp)
target_include_directories(hl_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hl_test_support PUBLIC hyperliquid GTest::gtest)
hl_target_defaults(hl_test_support)

set(HL_UNIT_TESTS
    decimal_test
    keccak_test
    signer_test
    actions_test
    asset_registry_test
    order_book_test
    ws_message_parser_test
    exchange_response_test
    ws_codec_test
    http_codec_test
    url_test
    event_loop_test
    exchange_client_test
    market_data_client_test
    scalar_test
    nonce_pool_test
)

add_executable(hl_tests)
foreach(t ${HL_UNIT_TESTS})
    target_sources(hl_tests PRIVATE ${t}.cpp)
endforeach()
target_link_libraries(hl_tests PRIVATE hyperliquid hl_test_support hl_secp256k1 GTest::gtest GTest::gtest_main)
target_include_directories(hl_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(hl_tests PRIVATE HL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
hl_target_defaults(hl_tests)
gtest_discover_tests(hl_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)

if(HL_BUILD_EXAMPLES)
    target_sources(hl_tests PRIVATE quote_engine_test.cpp)
    target_link_libraries(hl_tests PRIVATE hl_example_strategy)
endif()
