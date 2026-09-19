// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include <gtest/gtest.h>

#include <optional>
#include <random>
#include <string>
#include <vector>

#include "hl/WsMessageParser.h"
#include "support/Fixtures.h"

using hl::Decimal;

namespace {

Decimal d(std::string_view s) { return Decimal::parseOrZero(s); }

// Records owning copies of everything it receives.
struct Recorder : hl::WsMessageHandler {
    struct Book {
        std::string coin;
        std::int64_t time{};
        std::vector<hl::BookLevel> bids, asks;
    };
    std::vector<Book> books;
    std::vector<hl::BboMsg> bbos;
    std::vector<std::string> bboCoins;
    std::vector<hl::TradeMsg> trades;
    std::vector<std::string> tradeHashes;
    std::vector<hl::AssetCtxMsg> ctxs;
    std::size_t mids{0};
    Decimal btcMid;
    std::vector<hl::OrderUpdateMsg> orderUpdates;
    std::vector<std::string> statusTexts;
    std::vector<hl::FillMsg> fills;
    bool fillsSnapshot{false};
    std::vector<std::pair<std::uint64_t, std::string>> posts;
    std::vector<hl::PostResponseMsg::Type> postTypes;
    std::vector<std::string> subs;
    int pongs{0};
    std::vector<std::string> errors;
    std::vector<std::string> unhandled;
    std::vector<hl::CandleMsg> candles;
    std::vector<std::string> candleCoins;
    std::optional<hl::LiquidationMsg> liquidation;
    std::vector<hl::NonUserCancelMsg> nonUserCancels;
    std::vector<hl::UserFundingMsg> fundings;
    std::optional<hl::ActiveAssetDataMsg> activeAssetData;
    std::string notification;

    void onL2Book(const hl::L2BookMsg& m) override {
        books.push_back({std::string{m.coin}, m.timeMs, {m.bids.begin(), m.bids.end()}, {m.asks.begin(), m.asks.end()}});
    }
    void onBbo(const hl::BboMsg& m) override {
        bbos.push_back(m);
        bboCoins.emplace_back(m.coin);
    }
    void onTrades(std::span<const hl::TradeMsg> t) override {
        for (const auto& x : t) {
            trades.push_back(x);
            tradeHashes.emplace_back(x.hash);
        }
    }
    void onAssetCtx(const hl::AssetCtxMsg& m) override { ctxs.push_back(m); }
    void onAllMids(const hl::AllMidsMsg& m) override {
        mids = m.mids.size();
        for (const auto& e : m.mids) {
            if (e.coin == "BTC") {
                btcMid = e.mid;
            }
        }
    }
    void onOrderUpdates(std::span<const hl::OrderUpdateMsg> u) override {
        for (const auto& x : u) {
            orderUpdates.push_back(x);
            statusTexts.emplace_back(x.statusText);
        }
    }
    void onUserFills(const hl::UserFillsMsg& m) override {
        fillsSnapshot = m.isSnapshot;
        fills.assign(m.fills.begin(), m.fills.end());
    }
    void onPostResponse(const hl::PostResponseMsg& m) override {
        posts.emplace_back(m.id, std::string{m.payload});
        postTypes.push_back(m.type);
    }
    void onSubscriptionResponse(const hl::SubscriptionResponseMsg& m) override {
        subs.push_back(std::string{m.method} + ":" + std::string{m.subscriptionType} + ":" + std::string{m.coin});
    }
    void onPong() override { ++pongs; }
    void onVenueError(std::string_view t) override { errors.emplace_back(t); }
    void onUnhandled(std::string_view channel, std::string_view) override { unhandled.emplace_back(channel); }
    void onCandle(const hl::CandleMsg& m) override {
        candles.push_back(m);
        candleCoins.emplace_back(m.coin);
    }
    void onLiquidation(const hl::LiquidationMsg& m) override { liquidation = m; }
    void onNonUserCancels(std::span<const hl::NonUserCancelMsg> c) override {
        nonUserCancels.assign(c.begin(), c.end());
    }
    void onUserFundings(std::span<const hl::UserFundingMsg> f) override { fundings.assign(f.begin(), f.end()); }
    void onActiveAssetData(const hl::ActiveAssetDataMsg& m) override { activeAssetData = m; }
    void onNotification(const hl::NotificationMsg& m) override { notification = std::string{m.text}; }
};

}  // namespace

TEST(WsMessageParser, L2BookFixture) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(hltest::fixture("l2book_btc.json"), r));
    ASSERT_EQ(r.books.size(), 1U);
    const auto& b = r.books[0];
    EXPECT_EQ(b.coin, "BTC");
    EXPECT_EQ(b.time, 1789582828935);
    ASSERT_EQ(b.bids.size(), 20U);
    ASSERT_EQ(b.asks.size(), 20U);
    EXPECT_EQ(b.bids[0].px, d("75951"));
    EXPECT_EQ(b.bids[0].sz, d("0.4387"));
    EXPECT_EQ(b.bids[0].n, 1U);
    EXPECT_EQ(b.bids[1].n, 4U);
    EXPECT_EQ(b.asks[0].px, d("75952"));
    EXPECT_EQ(b.asks[0].sz, d("3.20035"));
    EXPECT_EQ(p.stats().messages, 1U);
    EXPECT_EQ(p.stats().parseErrors, 0U);
}

TEST(WsMessageParser, BboFixtureAndNullSide) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(hltest::fixture("bbo_btc.json"), r));
    ASSERT_TRUE(p.parse(R"({"channel":"bbo","data":{"coin":"ETH","time":5,"bbo":[null,{"px":"1.5","sz":"2","n":3}]}})", r));
    ASSERT_EQ(r.bbos.size(), 2U);
    EXPECT_EQ(r.bboCoins[0], "BTC");
    EXPECT_TRUE(r.bbos[0].hasBid && r.bbos[0].hasAsk);
    EXPECT_EQ(r.bbos[0].bid.px, d("75951"));
    EXPECT_EQ(r.bbos[0].ask.sz, d("4.43435"));
    EXPECT_EQ(r.bbos[0].ask.n, 13U);
    EXPECT_FALSE(r.bbos[1].hasBid);
    EXPECT_TRUE(r.bbos[1].hasAsk);
    EXPECT_EQ(r.bbos[1].ask.px, d("1.5"));
}

TEST(WsMessageParser, TradesFixture) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(hltest::fixture("trades_btc.json"), r));
    ASSERT_EQ(r.trades.size(), 30U);
    const auto& t = r.trades[0];
    EXPECT_EQ(t.side, hl::Side::Buy);
    EXPECT_EQ(t.px, d("75942"));
    EXPECT_EQ(t.sz, d("0.01"));
    EXPECT_EQ(t.timeMs, 1789582824979);
    EXPECT_EQ(t.tid, 137324211284779ULL);
    EXPECT_EQ(r.tradeHashes[0], "0x9ed09b4488e201c8a04a04448b9b11020681002a23e5209a4299469747e5dbb3");
}

TEST(WsMessageParser, AssetCtxFixture) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(hltest::fixture("asset_ctx_btc.json"), r));
    ASSERT_EQ(r.ctxs.size(), 1U);
    const auto& c = r.ctxs[0];
    EXPECT_FALSE(c.isSpot);
    EXPECT_EQ(c.funding, d("0.0000125"));
    EXPECT_EQ(c.openInterest, d("36851.8675"));
    EXPECT_EQ(c.premium, d("-0.00017113"));
    EXPECT_EQ(c.oraclePx, d("75965"));
    EXPECT_EQ(c.markPx, d("75933"));
    EXPECT_TRUE(c.hasMidPx);
    EXPECT_EQ(c.midPx, d("75951.5"));
    EXPECT_EQ(c.dayNtlVlm, d("3468613046.26649045"));
}

TEST(WsMessageParser, AllMidsFixture) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(hltest::fixture("all_mids.json"), r));
    EXPECT_EQ(r.mids, 1079U);
    EXPECT_EQ(r.btcMid, d("75981.5"));
}

TEST(WsMessageParser, ControlChannels) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(hltest::fixture("subscription_response.json"), r));
    ASSERT_TRUE(p.parse(hltest::fixture("pong.json"), r));
    ASSERT_TRUE(p.parse(R"({"channel":"error","data":"Invalid subscription"})", r));
    ASSERT_TRUE(p.parse(R"({"channel":"webData2","data":{"t":1}})", r));
    ASSERT_EQ(r.subs.size(), 1U);
    EXPECT_EQ(r.subs[0], "subscribe:l2Book:BTC");
    EXPECT_EQ(r.pongs, 1);
    ASSERT_EQ(r.errors.size(), 1U);
    EXPECT_EQ(r.errors[0], "Invalid subscription");
    ASSERT_EQ(r.unhandled.size(), 1U);
    EXPECT_EQ(r.unhandled[0], "webData2");
    EXPECT_EQ(p.stats().unhandled, 1U);
}

TEST(WsMessageParser, OrderUpdates) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"orderUpdates","data":[
            {"order":{"coin":"BTC","side":"B","limitPx":"75000.0","sz":"0.001","oid":123456789,"timestamp":1700000000000,
                      "origSz":"0.002","cloid":"0x0000000000000000000000000000abcd"},"status":"open","statusTimestamp":1700000000001},
            {"order":{"coin":"ETH","side":"A","limitPx":"3000.5","sz":"0.0","oid":987,"timestamp":1700000000002,
                      "origSz":"1.0"},"status":"reduceOnlyCanceled","statusTimestamp":1700000000003},
            {"order":{"coin":"ETH","side":"A","limitPx":"3000.5","sz":"1.0","oid":988,"timestamp":1,"origSz":"1.0","cloid":null},
             "status":"badAloPxRejected","statusTimestamp":2}]})",
        r));
    ASSERT_EQ(r.orderUpdates.size(), 3U);
    const auto& u = r.orderUpdates[0];
    EXPECT_EQ(u.side, hl::Side::Buy);
    EXPECT_EQ(u.limitPx, d("75000"));
    EXPECT_EQ(u.sz, d("0.001"));
    EXPECT_EQ(u.origSz, d("0.002"));
    EXPECT_EQ(u.oid, 123456789U);
    ASSERT_TRUE(u.cloid);
    EXPECT_EQ(u.cloid->low(), 0xabcdU);
    EXPECT_EQ(u.status, hl::OrderUpdateStatus::Open);
    EXPECT_EQ(u.statusTimestampMs, 1700000000001);
    EXPECT_EQ(r.orderUpdates[1].side, hl::Side::Sell);
    EXPECT_FALSE(r.orderUpdates[1].cloid);
    EXPECT_EQ(r.orderUpdates[1].status, hl::OrderUpdateStatus::Canceled);
    EXPECT_EQ(r.statusTexts[1], "reduceOnlyCanceled");
    EXPECT_EQ(r.orderUpdates[2].status, hl::OrderUpdateStatus::Rejected);
    EXPECT_FALSE(r.orderUpdates[2].cloid);
}

TEST(WsMessageParser, OrderStatusMapping) {
    using S = hl::OrderUpdateStatus;
    EXPECT_EQ(hl::parseOrderUpdateStatus("open"), S::Open);
    EXPECT_EQ(hl::parseOrderUpdateStatus("filled"), S::Filled);
    EXPECT_EQ(hl::parseOrderUpdateStatus("triggered"), S::Triggered);
    EXPECT_EQ(hl::parseOrderUpdateStatus("canceled"), S::Canceled);
    EXPECT_EQ(hl::parseOrderUpdateStatus("scheduledCancel"), S::Canceled);
    EXPECT_EQ(hl::parseOrderUpdateStatus("marginCanceled"), S::Canceled);
    EXPECT_EQ(hl::parseOrderUpdateStatus("selfTradeCanceled"), S::Canceled);
    EXPECT_EQ(hl::parseOrderUpdateStatus("rejected"), S::Rejected);
    EXPECT_EQ(hl::parseOrderUpdateStatus("perpMarginRejected"), S::Rejected);
    EXPECT_EQ(hl::parseOrderUpdateStatus("somethingNew"), S::Unknown);
}

TEST(WsMessageParser, UserFills) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"userFills","data":{"isSnapshot":true,"user":"0x14791697260e4c9a71f18484c9f997b308e59325","fills":[
            {"coin":"BTC","px":"75930.0","sz":"0.30618","side":"A","time":1789582997706,"startPosition":"0.30618",
             "dir":"Close Long","closedPnl":"-17.115462","hash":"0x5cf7","oid":547034114966,"crossed":false,
             "fee":"-0.51081","tid":51121598154065,"feeToken":"USDC","cloid":"0x00000000000000000000000000000007","twapId":null}]}})",
        r));
    EXPECT_TRUE(r.fillsSnapshot);
    ASSERT_EQ(r.fills.size(), 1U);
    const auto& f = r.fills[0];
    EXPECT_EQ(f.side, hl::Side::Sell);
    EXPECT_EQ(f.px, d("75930"));
    EXPECT_EQ(f.sz, d("0.30618"));
    EXPECT_EQ(f.startPosition, d("0.30618"));
    EXPECT_EQ(f.closedPnl, d("-17.115462"));
    EXPECT_EQ(f.fee, d("-0.51081"));
    EXPECT_FALSE(f.crossed);
    EXPECT_EQ(f.oid, 547034114966ULL);
    EXPECT_EQ(f.tid, 51121598154065ULL);
    ASSERT_TRUE(f.cloid);
    EXPECT_EQ(f.cloid->low(), 7U);
}

TEST(WsMessageParser, PostResponses) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"post","data":{"id":17,"response":{"type":"action","payload":{"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":5}}]}}}}}})",
        r));
    ASSERT_TRUE(p.parse(R"({"channel":"post","data":{"id":18,"response":{"type":"error","payload":"Too many requests"}}})", r));
    ASSERT_EQ(r.posts.size(), 2U);
    EXPECT_EQ(r.posts[0].first, 17U);
    EXPECT_EQ(r.postTypes[0], hl::PostResponseMsg::Type::Action);
    EXPECT_EQ(r.posts[0].second,
              R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":5}}]}}})");
    EXPECT_EQ(r.postTypes[1], hl::PostResponseMsg::Type::Error);
    EXPECT_EQ(r.posts[1].second, "Too many requests");
}

TEST(WsMessageParser, MalformedInputIsCountedNotFatal) {
    hl::WsMessageParser p;
    Recorder r;
    EXPECT_FALSE(p.parse("", r));
    EXPECT_FALSE(p.parse("{not json", r));
    EXPECT_FALSE(p.parse(R"({"data":1})", r));
    EXPECT_FALSE(p.parse(R"({"channel":"l2Book","data":"oops"})", r));
    EXPECT_FALSE(p.parse(R"({"channel":"trades","data":[{"coin":"BTC","px":"abc"}]})", r));
    EXPECT_EQ(p.stats().parseErrors, 5U);
    ASSERT_TRUE(p.parse(hltest::fixture("bbo_btc.json"), r));  // parser still usable
}

TEST(WsMessageParser, EntireCapturedSessionParses) {
    hl::WsMessageParser p;
    Recorder r;
    const auto lines = hltest::fixtureLines("session_mainnet.jsonl");
    ASSERT_GT(lines.size(), 500U);
    for (const auto& line : lines) {
        ASSERT_TRUE(p.parse(line, r)) << line.substr(0, 200);
    }
    EXPECT_EQ(p.stats().parseErrors, 0U);
    EXPECT_FALSE(r.books.empty());
    EXPECT_FALSE(r.bbos.empty());
    EXPECT_FALSE(r.trades.empty());
    EXPECT_FALSE(r.ctxs.empty());
}


TEST(WsMessageParser, Candle) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"candle","data":{"t":1789800000000,"T":1789800059999,"s":"ETH","i":"1m","o":"2629.4",)"
        R"("c":"2630.2","h":"2630.3","l":"2629.4","v":"59.6145","n":49}})",
        r));
    ASSERT_EQ(r.candles.size(), 1U);
    EXPECT_EQ(r.candleCoins[0], "ETH");
    EXPECT_EQ(r.candles[0].openTimeMs, 1789800000000LL);
    EXPECT_EQ(r.candles[0].closeTimeMs, 1789800059999LL);
    EXPECT_EQ(r.candles[0].open, d("2629.4"));
    EXPECT_EQ(r.candles[0].close, d("2630.2"));
    EXPECT_EQ(r.candles[0].high, d("2630.3"));
    EXPECT_EQ(r.candles[0].low, d("2629.4"));
    EXPECT_EQ(r.candles[0].volume, d("59.6145"));
    EXPECT_EQ(r.candles[0].trades, 49U);
}

// The `userEvents` subscription is delivered on a channel named "user" and carries four
// different payloads; liquidations and venue-initiated cancels appear nowhere else.
TEST(WsMessageParser, UserEventsLiquidation) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"user","data":{"liquidation":{"lid":42,"liquidator":"0xabc","liquidated_user":"0xdef",)"
        R"("liquidated_ntl_pos":"1234.5","liquidated_account_value":"-1.25"}}})",
        r));
    ASSERT_TRUE(r.liquidation);
    EXPECT_EQ(r.liquidation->lid, 42U);
    EXPECT_EQ(r.liquidation->liquidatedNtlPos, d("1234.5"));
    EXPECT_EQ(r.liquidation->liquidatedAccountValue, d("-1.25"));
}

TEST(WsMessageParser, UserEventsNonUserCancelAndFunding) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(R"({"channel":"user","data":{"nonUserCancel":[{"coin":"ETH","oid":7},{"coin":"BTC","oid":8}]}})", r));
    ASSERT_EQ(r.nonUserCancels.size(), 2U);
    EXPECT_EQ(r.nonUserCancels[0].oid, 7U);
    EXPECT_EQ(r.nonUserCancels[1].oid, 8U);

    ASSERT_TRUE(p.parse(
        R"({"channel":"user","data":{"funding":{"time":1789800000000,"coin":"ETH","usdc":"-0.0123","szi":"1.5",)"
        R"("fundingRate":"0.0000125"}}})",
        r));
    ASSERT_EQ(r.fundings.size(), 1U);
    EXPECT_EQ(r.fundings[0].coin, "ETH");
    EXPECT_EQ(r.fundings[0].usdc, d("-0.0123"));
    EXPECT_EQ(r.fundings[0].rate, d("0.0000125"));
}

TEST(WsMessageParser, UserEventsFillsReuseTheFillParser) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"user","data":{"fills":[{"coin":"BTC","px":"75930.0","sz":"0.3","side":"A","time":1789582997706,)"
        R"("startPosition":"0.3","dir":"Close Long","closedPnl":"-1.5","hash":"0xabc","oid":1,"crossed":true,)"
        R"("fee":"0.01","tid":2,"feeToken":"USDC"}]}})",
        r));
    ASSERT_EQ(r.fills.size(), 1U);
    EXPECT_FALSE(r.fillsSnapshot);
    EXPECT_EQ(r.fills[0].side, hl::Side::Sell);
    EXPECT_EQ(r.fills[0].tid, 2U);
}

TEST(WsMessageParser, UserFundingsChannel) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"userFundings","data":{"isSnapshot":true,"user":"0xabc","fundings":[)"
        R"({"time":1,"coin":"ETH","usdc":"0.5","szi":"-2","fundingRate":"0.00001"},)"
        R"({"time":2,"coin":"BTC","usdc":"-1.5","szi":"0.1","fundingRate":"0.00002"}]}})",
        r));
    ASSERT_EQ(r.fundings.size(), 2U);
    EXPECT_EQ(r.fundings[0].usdc, d("0.5"));
    EXPECT_EQ(r.fundings[1].coin, "BTC");
}

TEST(WsMessageParser, ActiveAssetDataAndNotification) {
    hl::WsMessageParser p;
    Recorder r;
    ASSERT_TRUE(p.parse(
        R"({"channel":"activeAssetData","data":{"user":"0xabc","coin":"ETH","leverage":{"type":"isolated","value":7},)"
        R"("maxTradeSzs":["10.5","11.25"],"availableToTrade":["900.0","910.0"],"markPx":"2640.5"}})",
        r));
    ASSERT_TRUE(r.activeAssetData);
    EXPECT_EQ(r.activeAssetData->coin, "ETH");
    EXPECT_EQ(r.activeAssetData->leverage, 7U);
    EXPECT_FALSE(r.activeAssetData->isCross);
    EXPECT_EQ(r.activeAssetData->maxTradeSzBuy, d("10.5"));
    EXPECT_EQ(r.activeAssetData->maxTradeSzSell, d("11.25"));
    EXPECT_EQ(r.activeAssetData->availableToTradeSell, d("910"));
    EXPECT_EQ(r.activeAssetData->markPx, d("2640.5"));

    ASSERT_TRUE(p.parse(R"({"channel":"notification","data":{"notification":"Your order was canceled"}})", r));
    EXPECT_EQ(r.notification, "Your order was canceled");
}

// Every fixture truncated at every length must be rejected cleanly (no crash, counted as an error).
TEST(WsMessageParser, TruncatedFramesNeverCrash) {
    hl::WsMessageParser p;
    Recorder r;
    std::size_t rejected = 0;
    std::size_t attempts = 0;
    for (const char* name : {"l2book_btc.json", "bbo_btc.json", "trades_btc.json", "asset_ctx_btc.json"}) {
        const std::string frame = hltest::fixture(name);
        for (std::size_t len = 0; len < frame.size(); ++len) {
            ++attempts;
            rejected += p.parse(std::string_view{frame}.substr(0, len), r) ? 0 : 1;
        }
    }
    EXPECT_GT(attempts, 1000U);
    // simdjson's On-Demand parser is lazy: a truncation that leaves every field the handler touches
    // intact can still parse. What matters is that nothing crashes and almost everything is caught.
    EXPECT_GT(rejected, attempts - attempts / 100) << "truncated frames must be rejected";
    // The parser is still usable afterwards.
    EXPECT_TRUE(p.parse(hltest::fixture("bbo_btc.json"), r));
}

// Random byte mutations of a valid frame must be rejected or parsed, never crash.
TEST(WsMessageParser, MutatedFramesAreHandled) {
    hl::WsMessageParser p;
    Recorder r;
    std::string frame = hltest::fixture("l2book_btc.json");
    std::mt19937 rng{12345};
    for (int i = 0; i < 2000; ++i) {
        std::string mutated = frame;
        const std::size_t pos = rng() % mutated.size();
        mutated[pos] = static_cast<char>(rng() % 256);
        (void)p.parse(mutated, r);  // must not crash; validity is not asserted
    }
    EXPECT_TRUE(p.parse(frame, r));
}
