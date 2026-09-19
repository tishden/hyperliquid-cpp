#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hl/core/Log.h"
#include "hl/md/MarketDataClient.h"
#include "support/Fixtures.h"
#include "support/MockVenue.h"

using hltest::runUntil;

namespace {

struct Listener : hl::MarketDataListener {
    int connected{0};
    int disconnected{0};
    int snapshots{0};
    int bboUpdates{0};
    std::size_t trades{0};
    int ctx{0};
    std::string lastBookCoin;
    void onConnected() override { ++connected; }
    void onDisconnected(std::string_view) override { ++disconnected; }
    void onBookUpdate(const hl::OrderBook& book, BookUpdate kind) override {
        lastBookCoin = book.coin();
        (kind == BookUpdate::Snapshot ? snapshots : bboUpdates)++;
    }
    void onTrades(std::span<const hl::TradeMsg> t) override { trades += t.size(); }
    void onAssetCtx(const hl::AssetCtxMsg&) override { ++ctx; }
};

class MarketDataClientTest : public ::testing::Test {
protected:
    void SetUp() override { hl::setLogLevel(hl::LogLevel::Error); }

    hl::MarketDataConfig config(std::int64_t pingMs = 20'000, std::int64_t staleMs = 60'000) {
        hl::MarketDataConfig cfg;
        cfg.urlOverride = venue.wsUrl();
        cfg.session.pingIntervalMs = pingMs;
        cfg.session.staleTimeoutMs = staleMs;
        cfg.session.reconnectMinDelayMs = 20;
        cfg.session.reconnectMaxDelayMs = 40;
        return cfg;
    }

    std::size_t countSubscribes(std::string_view needle) const {
        std::size_t n = 0;
        for (const auto& m : venue.wsLog) {
            n += (m.find(R"("method":"subscribe")") != std::string::npos && m.find(needle) != std::string::npos) ? 1 : 0;
        }
        return n;
    }

    hl::EventLoop loop;
    hltest::MockVenue venue{loop};
    Listener listener;
};

}  // namespace

TEST_F(MarketDataClientTest, SubscribesAndMaintainsBook) {
    hl::MarketDataClient md(loop, listener, config());
    md.subscribeBook("BTC");
    md.subscribeTrades("BTC");
    md.subscribeAssetCtx("BTC");
    EXPECT_EQ(md.book("ETH"), nullptr);
    ASSERT_NE(md.book("BTC"), nullptr);
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return venue.wsLog.size() == 4; }));
    EXPECT_EQ(listener.connected, 1);
    EXPECT_EQ(venue.wsLog[0], R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}})");
    EXPECT_EQ(venue.wsLog[1], R"({"method":"subscribe","subscription":{"type":"bbo","coin":"BTC"}})");

    venue.wsBroadcast(hltest::fixture("l2book_btc.json"));
    venue.wsBroadcast(hltest::fixture("bbo_btc.json"));
    venue.wsBroadcast(hltest::fixture("trades_btc.json"));
    venue.wsBroadcast(hltest::fixture("asset_ctx_btc.json"));
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ctx == 1; }));
    EXPECT_EQ(listener.snapshots, 1);
    EXPECT_EQ(listener.bboUpdates, 1);
    EXPECT_EQ(listener.trades, 30U);
    EXPECT_EQ(listener.lastBookCoin, "BTC");
    const hl::OrderBook* book = md.book("BTC");
    ASSERT_TRUE(book->isValid());
    EXPECT_EQ(book->bestAsk()->sz, hl::Decimal::parseOrZero("4.43435"));  // bbo overlay applied
    EXPECT_EQ(md.parserStats().parseErrors, 0U);
}

TEST_F(MarketDataClientTest, ReconnectReplaysSubscriptionsAndClearsBooks) {
    hl::MarketDataClient md(loop, listener, config());
    md.subscribeL2Book("BTC");
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return countSubscribes("l2Book") == 1; }));
    venue.wsBroadcast(hltest::fixture("l2book_btc.json"));
    ASSERT_TRUE(runUntil(loop, [&] { return md.book("BTC")->isValid(); }));

    md.subscribeTrades("ETH");  // added while connected → sent immediately
    ASSERT_TRUE(runUntil(loop, [&] { return countSubscribes("trades") == 1; }));

    venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.disconnected == 1; }));
    EXPECT_FALSE(md.book("BTC")->isValid());
    ASSERT_TRUE(runUntil(loop, [&] { return listener.connected == 2 && countSubscribes("trades") == 2; }));
    EXPECT_EQ(countSubscribes("l2Book"), 2U);
    EXPECT_EQ(md.reconnectCount(), 1U);
}

TEST_F(MarketDataClientTest, HeartbeatAndStaleDetection) {
    hl::MarketDataClient md(loop, listener, config(/*pingMs=*/30, /*staleMs=*/80));
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] {
        for (const auto& m : venue.wsLog) {
            if (m == R"({"method":"ping"})") {
                return true;
            }
        }
        return false;
    }));
    // Venue never answers → connection considered stale → reconnect.
    ASSERT_TRUE(runUntil(loop, [&] { return listener.connected >= 2; }, 3000));
    EXPECT_GE(listener.disconnected, 1);
}

TEST_F(MarketDataClientTest, AggregatedBookAndRawSubscriptions) {
    hl::MarketDataClient md(loop, listener, config());
    md.subscribeL2Book("ETH", hl::L2BookOptions{5, 2});
    md.subscribeRaw(R"({"type":"candle","coin":"BTC","interval":"1m"})");
    md.subscribeAllMids();
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return venue.wsLog.size() == 3; }));
    EXPECT_EQ(venue.wsLog[0], R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"ETH","nSigFigs":5,"mantissa":2}})");
    md.unsubscribeRaw(R"({"type":"candle","coin":"BTC","interval":"1m"})");
    ASSERT_TRUE(runUntil(loop, [&] { return venue.wsLog.size() == 4; }));
    EXPECT_EQ(venue.wsLog[3], R"({"method":"unsubscribe","subscription":{"type":"candle","coin":"BTC","interval":"1m"}})");
    EXPECT_EQ(hl::MarketDataClient::subscriptionJson("allMids"), R"({"type":"allMids"})");
}

// "localhost" resolves to ::1 and 127.0.0.1; the mock listens on IPv4 only, so every connect
// whose rotation starts at ::1 must fall through to the next address instead of failing.
TEST_F(MarketDataClientTest, FallsBackToNextResolvedAddress) {
    hl::MarketDataConfig cfg = config();
    cfg.urlOverride = "ws://localhost:" + std::to_string(venue.port()) + "/ws";
    hl::MarketDataClient md(loop, listener, cfg);
    md.subscribeTrades("BTC");
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.connected == 1; }));
    for (int i = 0; i < 3; ++i) {  // reconnects rotate the starting address
        venue.dropWebSockets();
        ASSERT_TRUE(runUntil(loop, [&] { return listener.connected == i + 2; })) << i;
    }
    EXPECT_EQ(listener.disconnected, 3);
}

TEST_F(MarketDataClientTest, ReconnectNowResubscribesImmediately) {
    hl::MarketDataClient md(loop, listener, config());
    md.subscribeTrades("BTC");
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return countSubscribes("trades") == 1; }));
    md.session().reconnectNow();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.connected == 2 && countSubscribes("trades") == 2; }));
    EXPECT_EQ(listener.disconnected, 1);
    EXPECT_EQ(md.reconnectCount(), 1U);
}

TEST_F(MarketDataClientTest, ReconnectNowWhileStillConnectingKeepsTheSessionAlive) {
    hl::MarketDataConfig cfg = config();
    // Point at a port nobody listens on so the session stays in the connecting state.
    cfg.urlOverride = "ws://127.0.0.1:9/ws";
    cfg.session.reconnectMinDelayMs = 20;
    hl::MarketDataClient md(loop, listener, cfg);
    md.subscribeTrades("BTC");
    md.start();
    loop.runOnce(20);
    md.session().reconnectNow();  // must not leave the session dead with no timer armed
    ASSERT_TRUE(runUntil(loop, [&] { return md.reconnectCount() >= 2; }, 5000))
        << "the session must keep retrying after reconnectNow() during a connect";
}

TEST_F(MarketDataClientTest, UnsubscribingAnL2BookDropsTheMaintainedBook) {
    hl::MarketDataClient md(loop, listener, config());
    md.subscribeL2Book("BTC");
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return countSubscribes("l2Book") == 1; }));
    venue.wsBroadcast(hltest::fixture("l2book_btc.json"));
    ASSERT_TRUE(runUntil(loop, [&] { return md.book("BTC") != nullptr && md.book("BTC")->isValid(); }));
    md.unsubscribeRaw(hl::MarketDataClient::subscriptionJson("l2Book", "BTC"));
    EXPECT_EQ(md.book("BTC"), nullptr) << "a book nobody feeds must not linger";
}

// `fast` is a parameter of the l2Book subscription, not a channel of its own: the venue answers on
// `l2Book` with the same frame shape, only five levels per side instead of twenty.
TEST_F(MarketDataClientTest, FastL2BookSubscriptionShape) {
    using hl::L2BookOptions;
    using hl::MarketDataClient;
    // Omitted when false, so strings built by earlier versions still match for unsubscribe.
    EXPECT_EQ(MarketDataClient::l2BookSubscriptionJson("BTC"), R"({"type":"l2Book","coin":"BTC"})");
    EXPECT_EQ(MarketDataClient::l2BookSubscriptionJson("BTC", L2BookOptions{.fast = false}),
              MarketDataClient::subscriptionJson("l2Book", "BTC"));
    EXPECT_EQ(MarketDataClient::l2BookSubscriptionJson("BTC", L2BookOptions{.fast = true}),
              R"({"type":"l2Book","coin":"BTC","fast":true})");
    EXPECT_EQ(MarketDataClient::l2BookSubscriptionJson("BTC", L2BookOptions{.nSigFigs = 5, .mantissa = 2, .fast = true}),
              R"({"type":"l2Book","coin":"BTC","nSigFigs":5,"mantissa":2,"fast":true})");
}

TEST_F(MarketDataClientTest, FastL2BookMaintainsTheBookAndUnsubscribesByTheSameString) {
    hl::MarketDataClient md(loop, listener, config());
    md.subscribeBook("BTC", hl::L2BookOptions{.fast = true});
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return venue.wsLog.size() == 2; }));
    EXPECT_EQ(venue.wsLog[0], R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC","fast":true}})");
    EXPECT_EQ(venue.wsLog[1], R"({"method":"subscribe","subscription":{"type":"bbo","coin":"BTC"}})");

    // A five-level snapshot, as the fast path publishes it.
    venue.wsBroadcast(
        R"({"channel":"l2Book","data":{"coin":"BTC","time":1789582828935,"levels":[)"
        R"([{"px":"75951.0","sz":"0.4","n":1},{"px":"75950.0","sz":"0.8","n":4},{"px":"75949.0","sz":"0.2","n":2},)"
        R"({"px":"75948.0","sz":"0.1","n":1},{"px":"75945.0","sz":"0.5","n":1}],)"
        R"([{"px":"75952.0","sz":"0.3","n":2},{"px":"75953.0","sz":"0.6","n":1},{"px":"75954.0","sz":"0.7","n":3},)"
        R"({"px":"75955.0","sz":"0.9","n":2},{"px":"75956.0","sz":"1.1","n":5}]]}})");
    ASSERT_TRUE(runUntil(loop, [&] { return listener.snapshots == 1; }));
    const hl::OrderBook* book = md.book("BTC");
    ASSERT_NE(book, nullptr);
    EXPECT_TRUE(book->isValid());
    EXPECT_EQ(book->bids().size(), 5U) << "the fast path is five levels deep, and the book keeps exactly those";
    EXPECT_EQ(book->asks().size(), 5U);
    EXPECT_EQ(book->bestBid()->px, hl::Decimal::parseOrZero("75951.0"));

    // bbo overlays onto a fast book exactly as onto a 20-level one.
    venue.wsBroadcast(hltest::fixture("bbo_btc.json"));
    ASSERT_TRUE(runUntil(loop, [&] { return listener.bboUpdates >= 1; }));

    md.unsubscribeRaw(hl::MarketDataClient::l2BookSubscriptionJson("BTC", hl::L2BookOptions{.fast = true}));
    EXPECT_EQ(md.book("BTC"), nullptr) << "the options-built string must round-trip through unsubscribe";
}

TEST_F(MarketDataClientTest, UserAndCandleSubscriptions) {
    hl::MarketDataClient md(loop, listener, config());
    const auto user = *hl::parseAddress("0x14791697260e4c9a71f18484c9f997b308e59325");
    md.subscribeCandle("ETH", "15m");
    md.subscribeUserEvents(user);
    md.subscribeUserFundings(user);
    md.subscribeActiveAssetData(user, "ETH");
    md.subscribeNotifications(user);
    md.start();
    ASSERT_TRUE(runUntil(loop, [&] { return venue.wsLog.size() == 5; }));
    EXPECT_EQ(venue.wsLog[0], R"({"method":"subscribe","subscription":{"type":"candle","coin":"ETH","interval":"15m"}})");
    EXPECT_NE(venue.wsLog[1].find(R"("type":"userEvents")"), std::string::npos);
    EXPECT_NE(venue.wsLog[3].find(R"({"type":"activeAssetData","user":"0x14791697260e4c9a71f18484c9f997b308e59325","coin":"ETH"})"),
              std::string::npos);
    // Invalid coin names are refused instead of producing malformed JSON.
    md.subscribeCandle("BAD\"COIN", "1m");
    md.subscribeL2Book("BAD\"COIN");
    loop.runOnce(20);
    EXPECT_EQ(venue.wsLog.size(), 5U);
}
