#include <gtest/gtest.h>

#include <vector>

#include "testnet_quoter/QuoteEngine.h"

using hl::Decimal;

namespace {

Decimal d(std::string_view s) { return Decimal::parseOrZero(s); }

hl::OrderBook ethBook(std::string_view bid = "2000", std::string_view ask = "2000.2") {
    hl::OrderBook b{"ETH"};
    const std::vector<hl::BookLevel> bids = {{d(bid), d("10"), 1}};
    const std::vector<hl::BookLevel> asks = {{d(ask), d("10"), 1}};
    b.applySnapshot(hl::L2BookMsg{"ETH", 1, bids, asks});
    return b;
}

hl::AssetInfo eth() {
    hl::AssetInfo a;
    a.name = "ETH";
    a.szDecimals = 4;
    return a;
}

example::QuoteParams params() {
    example::QuoteParams p;
    p.halfSpreadBps = 10;
    p.skewBps = 10;
    p.orderNotionalUsd = Decimal::fromInt(20);
    p.maxPositionUsd = Decimal::fromInt(100);
    p.useMicroprice = false;
    return p;
}

}  // namespace

TEST(QuoteEngine, SymmetricWhenFlat) {
    const auto q = example::computeQuotes(ethBook(), Decimal{}, eth(), params());
    ASSERT_TRUE(q.bidPx && q.askPx);
    EXPECT_EQ(q.fair, d("2000.1"));
    // 2000.1·(1∓0.001) = 1998.0999 / 2002.1001 → 5 significant figures → one decimal
    EXPECT_EQ(*q.bidPx, d("1998"));
    EXPECT_EQ(*q.askPx, d("2002.2"));
    EXPECT_EQ(q.size, d("0.0099"));    // 20 / 2000.1 = 0.009999… → down to 4 decimals
    EXPECT_DOUBLE_EQ(q.inventoryRatio, 0.0);
}

TEST(QuoteEngine, LongInventorySkewsDown) {
    const auto flat = example::computeQuotes(ethBook(), Decimal{}, eth(), params());
    const auto longQ = example::computeQuotes(ethBook(), d("0.025"), eth(), params());  // $50 → ratio 0.5
    ASSERT_TRUE(longQ.bidPx && longQ.askPx);
    EXPECT_NEAR(longQ.inventoryRatio, 0.5, 1e-3);
    EXPECT_LT(*longQ.bidPx, *flat.bidPx);
    EXPECT_LT(*longQ.askPx, *flat.askPx);
}

TEST(QuoteEngine, InventoryLimitDropsOneSide) {
    const auto longQ = example::computeQuotes(ethBook(), d("0.06"), eth(), params());  // $120 > $100
    EXPECT_FALSE(longQ.bidPx);
    EXPECT_TRUE(longQ.askPx);
    const auto shortQ = example::computeQuotes(ethBook(), d("-0.06"), eth(), params());
    EXPECT_TRUE(shortQ.bidPx);
    EXPECT_FALSE(shortQ.askPx);
}

TEST(QuoteEngine, NeverCrossesTheBook) {
    auto p = params();
    p.halfSpreadBps = 0.1;
    p.skewBps = 50;
    const auto q = example::computeQuotes(ethBook("2000", "2000.1"), d("-0.04"), eth(), p);  // short → shift up
    ASSERT_TRUE(q.bidPx && q.askPx);
    EXPECT_LT(*q.bidPx, d("2000.1"));
    EXPECT_GT(*q.askPx, d("2000"));
}

TEST(QuoteEngine, NoQuotesWithoutValidBook) {
    hl::OrderBook empty{"ETH"};
    const auto q = example::computeQuotes(empty, Decimal{}, eth(), params());
    EXPECT_FALSE(q.bidPx);
    EXPECT_FALSE(q.askPx);
}

TEST(QuoteEngine, DistanceBps) {
    EXPECT_NEAR(example::distanceBps(d("100.1"), d("100")), 10.0, 1e-9);
    EXPECT_EQ(example::distanceBps(d("1"), Decimal{}), 0.0);
}
