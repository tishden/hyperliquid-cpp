// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include <gtest/gtest.h>

#include "hl/om/AssetRegistry.h"
#include "support/Fixtures.h"

using hl::AssetInfo;
using hl::Decimal;
using hl::RoundingMode;

namespace {
Decimal d(std::string_view s) { return Decimal::parseOrZero(s); }

AssetInfo perp(int szDecimals) {
    AssetInfo a;
    a.name = "X";
    a.kind = AssetInfo::Kind::Perp;
    a.szDecimals = szDecimals;
    return a;
}
}  // namespace

TEST(AssetRegistry, LoadsTestnetPerpMeta) {
    hl::AssetRegistry reg;
    auto n = reg.loadPerpMeta(hltest::fixture("meta_testnet.json"));
    ASSERT_TRUE(n) << n.error().message;
    EXPECT_GT(n.value(), 100U);
    const AssetInfo* btc = reg.find("BTC");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->szDecimals, 5);
    EXPECT_EQ(btc->kind, AssetInfo::Kind::Perp);
    EXPECT_EQ(reg.findByAsset(btc->asset), btc);
    // Asset ids are positions in the universe and differ between networks — never hard-code them.
    EXPECT_EQ(reg.find("SOL")->asset, 0U);
    EXPECT_EQ(reg.find("NOPE"), nullptr);
}

TEST(AssetRegistry, LoadsSpotMeta) {
    hl::AssetRegistry reg;
    ASSERT_TRUE(reg.loadPerpMeta(hltest::fixture("meta_testnet.json")));
    auto n = reg.loadSpotMeta(hltest::fixture("spot_meta_testnet.json"));
    ASSERT_TRUE(n) << n.error().message;
    const AssetInfo* purr = reg.find("PURR/USDC");
    ASSERT_NE(purr, nullptr);
    EXPECT_EQ(purr->asset, 10000U);
    EXPECT_EQ(purr->kind, AssetInfo::Kind::Spot);
    EXPECT_EQ(purr->szDecimals, 0);
    EXPECT_NE(reg.find("BTC"), nullptr);  // perps survive spot load
}

TEST(AssetRegistry, RejectsGarbage) {
    hl::AssetRegistry reg;
    EXPECT_FALSE(reg.loadPerpMeta("not json"));
    EXPECT_FALSE(reg.loadPerpMeta(R"({"foo":1})"));
}

TEST(AssetInfo, PriceRoundingFiveSignificantFigures) {
    const AssetInfo btc = perp(5);  // max 1 decimal
    EXPECT_EQ(btc.roundPx(d("75951.37")), d("75951"));
    EXPECT_EQ(btc.roundPx(d("75951.5"), RoundingMode::Down), d("75951"));
    EXPECT_EQ(btc.roundPx(d("75951.5"), RoundingMode::Up), d("75952"));
    EXPECT_EQ(btc.roundPx(d("123456.7")), d("123457"));  // integers always allowed
    EXPECT_EQ(btc.roundPx(d("9999.99")), d("10000"));
    EXPECT_EQ(btc.roundPx(d("1234.56"), RoundingMode::Down), d("1234.5"));
    EXPECT_TRUE(btc.isValidPx(d("1234.5")));
    EXPECT_FALSE(btc.isValidPx(d("1234.56")));
    EXPECT_TRUE(btc.isValidPx(d("1234567")));
}

TEST(AssetInfo, PriceRoundingDecimalCapFromSzDecimals) {
    const AssetInfo eth = perp(4);  // max 6-4 = 2 decimals
    EXPECT_EQ(eth.roundPx(d("1670.123")), d("1670.1"));
    EXPECT_EQ(eth.roundPx(d("12.3456")), d("12.35"));
    const AssetInfo meme = perp(0);  // max 6 decimals
    EXPECT_EQ(meme.roundPx(d("0.0012345678"), RoundingMode::Down), d("0.001234"));
    EXPECT_EQ(meme.roundPx(d("0.0012345678"), RoundingMode::Up), d("0.001235"));
    EXPECT_EQ(meme.roundPx(d("0.123456789")), d("0.12346"));
    AssetInfo spot = perp(2);
    spot.kind = AssetInfo::Kind::Spot;  // max 8-2 = 6 decimals
    EXPECT_EQ(spot.maxPriceDecimals(), 6);
    EXPECT_EQ(spot.roundPx(d("0.00123456789"), RoundingMode::Down), d("0.001234"));
}

TEST(AssetInfo, SizeRounding) {
    const AssetInfo btc = perp(5);
    EXPECT_EQ(btc.roundSz(d("0.1234567")), d("0.12345"));
    EXPECT_EQ(btc.roundSz(d("0.1234567"), RoundingMode::Up), d("0.12346"));
    EXPECT_EQ(btc.roundSz(d("-0.1234567")), d("-0.12345"));
    EXPECT_TRUE(btc.isValidSz(d("0.00001")));
    EXPECT_FALSE(btc.isValidSz(d("0.000001")));
    const AssetInfo whole = perp(0);
    EXPECT_EQ(whole.roundSz(d("17.9")), d("17"));
}
