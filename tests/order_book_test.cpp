#include <gtest/gtest.h>

#include <vector>

#include "hl/WsMessageParser.h"
#include "hl/md/OrderBook.h"
#include "support/Fixtures.h"

using hl::BookLevel;
using hl::Decimal;

namespace {

Decimal d(std::string_view s) { return Decimal::parseOrZero(s); }

BookLevel lvl(std::string_view px, std::string_view sz, std::uint32_t n = 1) { return {d(px), d(sz), n}; }

hl::L2BookMsg snapshot(const std::vector<BookLevel>& bids, const std::vector<BookLevel>& asks, std::int64_t t = 1000) {
    return hl::L2BookMsg{"BTC", t, bids, asks};
}

hl::BboMsg bbo(std::string_view bidPx, std::string_view bidSz, std::string_view askPx, std::string_view askSz,
               std::int64_t t = 2000) {
    hl::BboMsg m;
    m.coin = "BTC";
    m.timeMs = t;
    m.hasBid = true;
    m.hasAsk = true;
    m.bid = lvl(bidPx, bidSz);
    m.ask = lvl(askPx, askSz);
    return m;
}

hl::OrderBook book() {
    hl::OrderBook b{"BTC"};
    const std::vector<BookLevel> bids = {lvl("100", "1"), lvl("99", "2"), lvl("98", "3")};
    const std::vector<BookLevel> asks = {lvl("101", "1"), lvl("102", "2"), lvl("103", "3")};
    b.applySnapshot(snapshot(bids, asks));
    return b;
}

}  // namespace

TEST(OrderBook, SnapshotAndDerivedPrices) {
    const auto b = book();
    ASSERT_TRUE(b.isValid());
    EXPECT_EQ(b.bids().size(), 3U);
    EXPECT_EQ(b.bestBid()->px, d("100"));
    EXPECT_EQ(b.bestAsk()->px, d("101"));
    EXPECT_EQ(b.mid(), d("100.5"));
    EXPECT_EQ(b.spread(), d("1"));
    EXPECT_NEAR(b.spreadBps(), 99.5, 0.01);
    EXPECT_EQ(b.microprice(), d("100.5"));
    EXPECT_EQ(b.cumulativeSize(hl::Side::Buy, 2), d("3"));
    EXPECT_EQ(b.cumulativeSize(hl::Side::Sell, 10), d("6"));
    EXPECT_EQ(b.snapshotTimeMs(), 1000);
}

TEST(OrderBook, MicropriceLeansToThinSide) {
    hl::OrderBook b{"BTC"};
    const std::vector<BookLevel> bids = {lvl("100", "9")};
    const std::vector<BookLevel> asks = {lvl("101", "1")};
    b.applySnapshot(snapshot(bids, asks));
    EXPECT_EQ(b.microprice(), d("100.9"));
}

TEST(OrderBook, VwapForSize) {
    const auto b = book();
    EXPECT_EQ(*b.vwapForSize(hl::Side::Buy, d("1")), d("101"));
    EXPECT_EQ(*b.vwapForSize(hl::Side::Buy, d("2")), d("101.5"));
    EXPECT_EQ(*b.vwapForSize(hl::Side::Sell, d("3")), d("99.33333333"));
    EXPECT_FALSE(b.vwapForSize(hl::Side::Buy, d("7")));
    EXPECT_FALSE(b.vwapForSize(hl::Side::Buy, d("0")));
}

TEST(OrderBook, BboResizesExistingTop) {
    auto b = book();
    ASSERT_TRUE(b.applyBbo(bbo("100", "5", "101", "0.5")));
    EXPECT_EQ(b.bids()[0].sz, d("5"));
    EXPECT_EQ(b.asks()[0].sz, d("0.5"));
    EXPECT_EQ(b.bids().size(), 3U);
    EXPECT_EQ(b.timeMs(), 2000);
}

TEST(OrderBook, BboImprovesInsideSpread) {
    auto b = book();
    ASSERT_TRUE(b.applyBbo(bbo("100.5", "0.1", "100.7", "0.2")));
    EXPECT_EQ(b.bids().size(), 4U);
    EXPECT_EQ(b.bids()[0].px, d("100.5"));
    EXPECT_EQ(b.bids()[1].px, d("100"));
    EXPECT_EQ(b.asks().size(), 4U);
    EXPECT_EQ(b.asks()[0].px, d("100.7"));
    EXPECT_EQ(b.asks()[1].px, d("101"));
    EXPECT_TRUE(b.isValid());
}

TEST(OrderBook, BboWalksThroughLevels) {
    auto b = book();
    // Bids consumed down to 98.5; asks moved down to 99 (removing stale bids >= 99).
    ASSERT_TRUE(b.applyBbo(bbo("98.5", "1", "99", "4")));
    ASSERT_EQ(b.bids().size(), 2U);
    EXPECT_EQ(b.bids()[0].px, d("98.5"));
    EXPECT_EQ(b.bids()[1].px, d("98"));
    ASSERT_EQ(b.asks().size(), 4U);
    EXPECT_EQ(b.asks()[0].px, d("99"));
    EXPECT_TRUE(b.isValid());
}

TEST(OrderBook, BboDropsSeveralLevelsToExistingPrice) {
    auto b = book();
    ASSERT_TRUE(b.applyBbo(bbo("98", "7", "103", "1")));
    ASSERT_EQ(b.bids().size(), 1U);
    EXPECT_EQ(b.bids()[0].sz, d("7"));
    ASSERT_EQ(b.asks().size(), 1U);
    EXPECT_EQ(b.asks()[0].px, d("103"));
}

TEST(OrderBook, StaleBboIgnoredAndEmptySide) {
    auto b = book();
    EXPECT_FALSE(b.applyBbo(bbo("50", "1", "51", "1", 999)));
    EXPECT_EQ(b.bestBid()->px, d("100"));
    hl::BboMsg m = bbo("100", "1", "101", "1");
    m.hasAsk = false;
    ASSERT_TRUE(b.applyBbo(m));
    EXPECT_FALSE(b.isValid());
    EXPECT_EQ(b.mid(), Decimal{});
    EXPECT_EQ(b.microprice(), Decimal{});
}

TEST(OrderBook, CapacityIsBounded) {
    hl::OrderBook b{"BTC"};
    std::vector<BookLevel> bids;
    for (int i = 0; i < 100; ++i) {
        bids.push_back(BookLevel{Decimal::fromInt(1000 - i), Decimal::fromInt(1), 1});
    }
    const std::vector<BookLevel> asks = {lvl("2000", "1")};
    b.applySnapshot(snapshot(bids, asks));
    EXPECT_EQ(b.bids().size(), hl::OrderBook::kMaxLevels);
    EXPECT_TRUE(b.applyBbo(bbo("1000.5", "1", "2000", "1")));
    EXPECT_EQ(b.bids().size(), hl::OrderBook::kMaxLevels);
    EXPECT_EQ(b.bids()[1].px, Decimal::fromInt(1000));
}

TEST(OrderBook, RealMainnetSnapshot) {
    struct Capture : hl::WsMessageHandler {
        hl::OrderBook book{"BTC"};
        void onL2Book(const hl::L2BookMsg& msg) override { book.applySnapshot(msg); }
    } capture;
    hl::WsMessageParser parser;
    ASSERT_TRUE(parser.parse(hltest::fixture("l2book_btc.json"), capture));
    EXPECT_TRUE(capture.book.isValid());
    EXPECT_EQ(capture.book.bids().size(), 20U);
    EXPECT_EQ(capture.book.asks().size(), 20U);
    EXPECT_EQ(capture.book.bestBid()->px, d("75951"));
    for (std::size_t i = 1; i < capture.book.bids().size(); ++i) {
        EXPECT_LT(capture.book.bids()[i].px, capture.book.bids()[i - 1].px);
        EXPECT_GT(capture.book.asks()[i].px, capture.book.asks()[i - 1].px);
    }
}

// A locked or crossed bbo is resolved by dropping the crossing levels: the book never reports a
// bid at or above its ask.
TEST(OrderBook, LockedAndCrossedBboNeverLeaveACrossedBook) {
    auto locked = book();
    ASSERT_TRUE(locked.applyBbo(bbo("100.5", "1", "100.5", "1")));  // bid == ask
    if (locked.isValid()) {
        EXPECT_LT(locked.bestBid()->px, locked.bestAsk()->px);
    }

    auto crossed = book();
    ASSERT_TRUE(crossed.applyBbo(bbo("101", "1", "100", "1")));  // bid > ask
    if (crossed.isValid()) {
        EXPECT_LT(crossed.bestBid()->px, crossed.bestAsk()->px);
    }
    // A later snapshot restores the venue's view either way.
    const std::vector<BookLevel> bids = {lvl("100", "1")};
    const std::vector<BookLevel> asks = {lvl("101", "1")};
    crossed.applySnapshot(snapshot(bids, asks, 5000));
    EXPECT_TRUE(crossed.isValid());
    EXPECT_EQ(crossed.bestBid()->px, d("100"));
}

// A side with no best price in the update is emptied until the next snapshot.
TEST(OrderBook, BboWithoutASideEmptiesIt) {
    auto b = book();
    hl::BboMsg m = bbo("100", "1", "101", "1");
    m.hasBid = false;
    ASSERT_TRUE(b.applyBbo(m));
    EXPECT_TRUE(b.bids().empty());
    EXPECT_FALSE(b.isValid());
}
