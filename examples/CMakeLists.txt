# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Denis Tishkov <denis8825@ya.ru>
add_library(hl_example_strategy STATIC testnet_quoter/QuoteEngine.cpp)
target_include_directories(hl_example_strategy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hl_example_strategy PUBLIC hyperliquid)
hl_target_defaults(hl_example_strategy)

add_executable(hl_testnet_quoter testnet_quoter/main.cpp testnet_quoter/Quoter.cpp)
target_include_directories(hl_testnet_quoter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hl_testnet_quoter PRIVATE hl_example_strategy)
hl_target_defaults(hl_testnet_quoter)

add_executable(hl_book_printer book_printer/main.cpp)
target_link_libraries(hl_book_printer PRIVATE hyperliquid)
hl_target_defaults(hl_book_printer)

add_executable(hl_live_check live_check/main.cpp)
target_include_directories(hl_live_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hl_live_check PRIVATE hyperliquid)
hl_target_defaults(hl_live_check)
